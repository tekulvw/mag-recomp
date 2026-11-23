"""Apply a hash-pinned retail Lua override only inside a disposable run overlay.

A profile may also name an account portal (the "portal" block) for a server that requires
tickets: the run then signs in with a ticket fetched from that portal for the server, as a linked
player's launch does (docs/player-settings.md, "Account link").
"""
import base64
import binascii
import hashlib
import ipaddress
import json
import os
import re
import socket
import ssl
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from tools import psarc
from tools.prepare_inputs import verify_hash, sha256_file

RETAIL_ELF_SHA256='eb916f92e7c32a9401739d79835630273e1f41a92e6a5d17753d9374062da7d9'
ARCHIVE_PATH=Path('PS3_GAME/USRDIR/run/mag/ps3/game.psarc.sdat')
MEMBER='/run/mag/ps3/settings/maglocalsettings.lua'


# ---- The portal block ----
# The portal's device tokens and the ticket layout, as src/player_portal.cpp checks them.
DEVICE_TOKEN=re.compile(r'mgd_[A-Za-z0-9_-]{4,252}')
HOST_NAME=re.compile(r'[a-z0-9](?:[a-z0-9-]*[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]*[a-z0-9])?)*')
PLAYER_NAME=re.compile(r'[A-Za-z][A-Za-z0-9_-]{2,14}')
PORTAL_TICKET='portal-ticket.bin'


class PortalError(ValueError):
    """A ticket could not be fetched; the message is for the operator and never holds the token."""


def check_portal_url(url) -> str:
    """The portal address, normalised: https://host[:port][/path]; plain http only for localhost."""
    if not isinstance(url, str) or len(url.strip()) > 256:
        raise ValueError('Portal url must be an https:// URL')
    url=url.strip()
    if any(c <= ' ' or c >= '\x7f' or c in '\\?#' for c in url):
        raise ValueError('Portal url must be an https:// URL without a query')
    parts=urllib.parse.urlsplit(url)
    if parts.netloc.endswith(':'):
        raise ValueError('Portal url has an invalid port')
    scheme=parts.scheme.lower()
    try:
        host,port=parts.hostname,parts.port
    except ValueError:
        raise ValueError('Portal url has an invalid port') from None
    if scheme not in ('https','http') or not host or parts.username is not None or parts.password is not None \
            or not HOST_NAME.fullmatch(host) or len(host) > 253 or port == 0:
        raise ValueError('Portal url must be an https:// URL such as https://magtest.online')
    if scheme=='http' and host not in ('localhost','127.0.0.1'):
        raise ValueError('Portal url must use https:// (plain http is only for localhost)')
    default=port is None or port==(443 if scheme=='https' else 80)
    return f'{scheme}://{host}{"" if default else f":{port}"}{parts.path.rstrip("/")}'


def check_audience(host) -> str:
    """A ticket audience: the game server's host name or IPv4 address, lowercased (at most 63 bytes)."""
    if not isinstance(host, str) or not 1 <= len(host) <= 63 or not HOST_NAME.fullmatch(host.lower()):
        raise ValueError('Portal audience must be the game server host name')
    return host.lower()


def check_portal(block, profile_path: Path, maps_host: str) -> dict:
    if not isinstance(block, dict) or not {'url','token_file'} <= set(block) <= {'url','token_file','audience'}:
        raise ValueError('Portal block must hold url, token_file and an optional audience')
    if not isinstance(block['token_file'], str) or not block['token_file']:
        raise ValueError('Portal token_file must be a path')
    token_file=Path(block['token_file']).expanduser()
    if not token_file.is_absolute(): token_file=profile_path.parent/token_file
    return {'url':check_portal_url(block['url']), 'token_file':str(token_file),
            'audience':check_audience(block.get('audience', maps_host))}


def read_token(path: Path) -> str:
    """The device token in `path` (surrounding white space ignored); the file must be private."""
    try:
        info=path.stat()
        if not path.is_file() or info.st_size > 4096:
            raise PortalError(f'Portal token file is not a small regular file: {path}')
        if os.name=='posix' and info.st_mode & 0o077:
            raise PortalError(f'Portal token file must be private to its owner (chmod 600): {path}')
        token=path.read_text(encoding='ascii').strip()
    except (OSError, UnicodeDecodeError) as error:
        raise PortalError(f'Cannot read the portal token file {path}: {type(error).__name__}') from None
    if not DEVICE_TOKEN.fullmatch(token):
        raise PortalError(f'Portal token file holds no device token (mgd_...): {path}')
    return token


def check_ticket(ticket: bytes, audience: str, player_name: str) -> None:
    """The version 1 layout (docs/portal-ticket.md): length 108 + A + N, magic and version, and the
    audience and name fields matching the reply. The signature is the game server's to verify."""
    if not 112 <= len(ticket) <= 186 or ticket[:4]!=b'MAGT' or ticket[4]!=1:
        raise PortalError('The account portal sent a ticket MAG cannot use')
    a=ticket[42]
    n=ticket[43+a] if 1 <= a <= 63 and 44+a <= len(ticket) else 0
    if not 3 <= n <= 15 or len(ticket)!=108+a+n or ticket[43:43+a]!=audience.encode() or \
            ticket[44+a:44+a+n]!=player_name.encode() or not PLAYER_NAME.fullmatch(player_name):
        raise PortalError('The account portal sent a ticket MAG cannot use')


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):  # the token goes to the portal and nowhere else
        return None


def write_private(path: Path, data: bytes) -> None:
    """Replace `path` atomically with an owner-only (0600) file."""
    temporary=path.with_name(path.name+'.new')
    try: temporary.unlink()
    except FileNotFoundError: pass
    fd=os.open(temporary, os.O_WRONLY|os.O_CREAT|os.O_EXCL|getattr(os,'O_BINARY',0), 0o600)
    try:
        with os.fdopen(fd,'wb') as stream:
            stream.write(data); stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary,path)
    except BaseException:
        temporary.unlink(missing_ok=True); raise


def fetch_portal_ticket(portal: dict, destination: Path, timeout: float = 20) -> dict:
    """POST <url>/api/v1/ticket for the audience with the profile's device token, check the reply,
    and write the ticket owner-only to `destination`. TLS is verified against the system store; no
    proxy and no redirect is used."""
    url=portal['url']; audience=portal['audience']
    token=read_token(Path(portal['token_file']))
    request=urllib.request.Request(url+'/api/v1/ticket', data=json.dumps({'audience':audience}).encode(), method='POST',
                                   headers={'Authorization':'Bearer '+token,'Content-Type':'application/json',
                                            'Accept':'application/json'})
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}), _NoRedirect(),
                                       urllib.request.HTTPSHandler(context=ssl.create_default_context()))
    try:
        with opener.open(request, timeout=timeout) as response:
            status,body=response.status,response.read(65537)
    except urllib.error.HTTPError as error:
        with error:
            status,body=error.code,error.read(65537)
    except (urllib.error.URLError, OSError) as error:
        reason=getattr(error,'reason',error)
        raise PortalError(f'Cannot reach the account portal at {url}: {type(reason).__name__}: {reason}') from None
    try:
        reply=json.loads(body) if len(body) <= 65536 else None
    except ValueError:
        reply=None
    reply=reply if isinstance(reply, dict) else {}
    code=reply.get('error')
    if status==200:
        ticket_text,name=reply.get('ticket'),reply.get('player_name')
        expires,account=reply.get('expires_at'),reply.get('account_id')
        if not (isinstance(ticket_text,str) and isinstance(name,str) and type(expires) is int and type(account) is int and account > 0):
            raise PortalError(f'The account portal at {url} sent an unexpected ticket reply')
        if reply.get('audience')!=audience:
            raise PortalError(f'The account portal at {url} sent a ticket for {str(reply.get("audience"))[:64]!r}, not {audience}')
        try:
            ticket=base64.b64decode(ticket_text, validate=True)
        except (binascii.Error, ValueError):
            raise PortalError(f'The account portal at {url} sent an unexpected ticket reply') from None
        check_ticket(ticket, audience, name)
        write_private(destination, ticket)
        return {'file':str(destination),'player_name':name,'audience':audience,'expires_at':expires,'account_id':account}
    if status==400 and code=='unknown_audience':
        raise PortalError(f"This account portal ({url}) doesn't issue tickets for {audience}")
    if status==401 and code=='invalid_token':
        raise PortalError(f'The account portal at {url} does not accept the token in {portal["token_file"]} (it may have been revoked)')
    if status==403 and code=='banned':
        raise PortalError(f'The portal account is banned from online play: {str(reply.get("reason") or "")[:200]!r}')
    if status==409 and code=='no_player_name':
        raise PortalError(f'The portal account has no player name yet: claim one at {url}/account')
    raise PortalError(f'The account portal at {url} sent an unexpected reply (HTTP {status})')


def server_address(host, ticket_server: bool) -> ipaddress.IPv4Address:
    """The MAPS server address. Loopback replacement services only, unless the profile names a
    ticket-enforcing server through a portal: that may be a LAN host (a private IPv4 address, or a
    name resolving to one), never a public one."""
    if not ticket_server:
        address=ipaddress.IPv4Address(host)
        if not address.is_loopback:
            raise ValueError('This diagnostic profile supports loopback replacement services only')
        return address
    if not isinstance(host, str) or not HOST_NAME.fullmatch(host.lower()):
        raise ValueError('Server host must be a host name or IPv4 address')
    try:
        address=ipaddress.IPv4Address(host)
    except ValueError:
        try:
            address=ipaddress.IPv4Address(socket.getaddrinfo(host, None, socket.AF_INET, socket.SOCK_STREAM)[0][4][0])
        except (OSError, IndexError):
            raise ValueError(f'Server host does not resolve to IPv4: {host}') from None
    if not (address.is_loopback or address.is_private) or address.is_unspecified:
        raise ValueError('A ticket profile reaches loopback or LAN (private IPv4) servers only')
    return address


def prepare_overlay(vfs: Path, profile_path: Path, expected_member: str = MEMBER) -> dict:
    raw=profile_path.read_bytes(); profile=json.loads(raw)
    if profile.get('format')!=1 or profile.get('elf_sha256')!=RETAIL_ELF_SHA256:
        raise ValueError('Local network profile must identify retail v1.0 exactly')
    portal=profile.get('portal')
    address=server_address(profile.get('maps_host'), portal is not None)
    for key in ('maps_tcp_port','maps_udp_port'):
        if type(profile.get(key)) is not int or not 1<=profile[key]<=65535:
            raise ValueError('Invalid local service port')
    for key in ('host_sockets','restore_ticket_setting'):
        if type(profile.get(key)) is not bool:
            raise ValueError('Network policy must be explicit booleans')
    account=profile.get('local_account_name')
    if account is not None and (account not in ('Player','PlayerRetail') or not profile['host_sockets'] or not profile['restore_ticket_setting']):
        raise ValueError('Local development identity requires the explicit no-ticket host-socket profile')
    local_ticket=profile.get('local_np_ticket',False)
    if type(local_ticket) is not bool or (local_ticket and not account):
        raise ValueError('Local NP ticket requires an explicit boolean and account identity')
    if portal is not None:
        # The portal ticket profile: host sockets, the game's own NP ticket setting kept, and the
        # ticket's account as the identity, so neither the no-ticket nor the local ticket profile.
        if not profile['host_sockets'] or profile['restore_ticket_setting'] or account is not None or local_ticket:
            raise ValueError('A portal ticket profile needs host_sockets, no restore_ticket_setting, '
                             'no local_account_name and no local_np_ticket')
        portal=check_portal(portal, profile_path, profile['maps_host'])
    if profile.get('member')!=expected_member:
        raise ValueError('Unexpected settings member')
    archive=Path(profile['archive'])
    if not archive.is_absolute(): archive=profile_path.parent/archive
    verify_hash(archive,profile['archive_sha256'])
    source=archive.read_bytes()
    _,names,extract,*_=psarc.parse(source)
    if names.count(expected_member)!=1:
        raise ValueError('Settings member must be unique')
    original=extract(names.index(expected_member)+1)
    if hashlib.sha256(original).hexdigest()!=profile['member_sha256']:
        raise ValueError('Settings member identity mismatch')
    settings=('MAGClientNetConfig = {\n'
              '  EnableNetwork = true,\n  EnableServerOnClient = false,\n'
              '  UseTestClient = false,\n  UseNPTicket = false,\n'
              f'  MAPSServerAddr = "{address}",\n'
              f'  MAPSServerTCPPort = {profile["maps_tcp_port"]},\n'
              f'  MAPSServerUDPPort = {profile["maps_udp_port"]},\n}}\n').encode()
    modified,change=psarc.replace(source,expected_member,settings)
    target=vfs/ARCHIVE_PATH
    if target.is_symlink() or not target.resolve().is_relative_to(vfs.resolve()):
        raise ValueError('Archive must stay inside the private overlay')
    verify_hash(target,profile['source_sdat_sha256'])
    target.write_bytes(modified)
    return {'profile_sha256':hashlib.sha256(raw).hexdigest(),
            'settings_sha256':hashlib.sha256(settings).hexdigest(),
            'settings_text':settings.decode(), 'archive_change':change,
            'archive_path':str(ARCHIVE_PATH), 'host_sockets':profile['host_sockets'],
            'restore_ticket_setting':profile['restore_ticket_setting'],
            'local_account_name':account,
            'local_np_ticket':local_ticket,
            'portal':portal,
            'np_status_policy':(f'portal ticket for {portal["audience"]}; no PSN authentication' if portal else
                                'local development ticket; no PSN authentication' if local_ticket else 'offline; no PSNR backend'),
            'expected_application_id':20231}
