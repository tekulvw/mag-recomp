"""Apply a hash-pinned retail Lua override only inside a disposable run overlay."""
import hashlib
import ipaddress
import json
from pathlib import Path
from tools import psarc
from tools.prepare_inputs import verify_hash, sha256_file

RETAIL_ELF_SHA256='eb916f92e7c32a9401739d79835630273e1f41a92e6a5d17753d9374062da7d9'
ARCHIVE_PATH=Path('PS3_GAME/USRDIR/run/mag/ps3/game.psarc.sdat')
MEMBER='/run/mag/ps3/settings/maglocalsettings.lua'


def prepare_overlay(vfs: Path, profile_path: Path, expected_member: str = MEMBER) -> dict:
    raw=profile_path.read_bytes(); profile=json.loads(raw)
    if profile.get('format')!=1 or profile.get('elf_sha256')!=RETAIL_ELF_SHA256:
        raise ValueError('Local network profile must identify retail v1.0 exactly')
    address=ipaddress.IPv4Address(profile['maps_host'])
    if not address.is_loopback:
        raise ValueError('This diagnostic profile supports loopback replacement services only')
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
            'np_status_policy':('local development ticket; no PSN authentication' if local_ticket else 'offline; no PSNR backend'),
            'expected_application_id':20231}
