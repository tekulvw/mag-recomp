"""Strict, versioned maintainer-owned profiles and exact host adapters."""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path, PurePosixPath
import platform
import re


class InstallerError(Exception):
    """A user-visible, fail-closed installer failure."""


HOSTS = frozenset(f'{system}-{arch}' for system in ('windows', 'linux', 'macos')
                  for arch in ('x86_64', 'arm64'))


def host_key(system=None, machine=None):
    system = system or platform.system()
    machine = (machine or platform.machine()).lower()
    systems = {'Windows': 'windows', 'Linux': 'linux', 'Darwin': 'macos'}
    arches = {'amd64': 'x86_64', 'x86_64': 'x86_64', 'arm64': 'arm64', 'aarch64': 'arm64'}
    if system not in systems or machine not in arches:
        raise InstallerError(f'Unsupported host: {system} / {machine}')
    return f'{systems[system]}-{arches[machine]}'


def relative_path(value):
    if not isinstance(value, str) or not value or '\\' in value or ':' in value or '\x00' in value:
        raise InstallerError('Expected a portable relative path')
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ('', '.', '..') for part in value.split('/')):
        raise InstallerError('Path must stay inside its root')
    reserved = {'CON', 'PRN', 'AUX', 'NUL'} | {f'{p}{i}' for p in ('COM', 'LPT') for i in range(1, 10)}
    for part in path.parts:
        if part.endswith(('.', ' ')) or part.split('.')[0].upper() in reserved or any(ord(c) < 32 or c in '<>"|?*' for c in part):
            raise InstallerError('Path is not portable across supported hosts')
    return value


def keys(value, required):
    if not isinstance(value, dict) or set(value) != set(required):
        raise InstallerError(f'Invalid profile fields; expected {sorted(required)}')


def string(value, label):
    if not isinstance(value, str) or not value.strip() or '\x00' in value:
        raise InstallerError(f'Invalid {label}')


def positive_int(value, label, zero=False):
    if type(value) is not int or value < (0 if zero else 1):
        raise InstallerError(f'Invalid {label}')


def sha256(value):
    if not isinstance(value, str) or not re.fullmatch('[a-f0-9]{64}', value):
        raise InstallerError('Expected a lowercase SHA-256')


@dataclass(frozen=True)
class Profile:
    data: dict
    digest: str

    @classmethod
    def load(cls, path):
        try:
            raw = Path(path).read_bytes()
            if len(raw) > 1024 * 1024:
                raise InstallerError('Profile exceeds 1 MiB')
            return cls.from_dict(json.loads(raw))
        except (OSError, ValueError) as exc:
            raise InstallerError(f'Cannot load profile: {exc}') from exc

    @classmethod
    def from_dict(cls, data):
        # Round trip removes shared mutable caller state and rejects non-JSON types.
        try:
            data = json.loads(json.dumps(data, allow_nan=False))
        except (ValueError, TypeError) as exc:
            raise InstallerError('Profile must be finite JSON') from exc
        keys(data, {'schema_version', 'id', 'revision', 'kind', 'approval', 'identity', 'platforms'})
        if type(data['schema_version']) is not int or data['schema_version'] != 1:
            raise InstallerError('Unsupported profile schema version')
        if not isinstance(data['id'], str) or not re.fullmatch('[a-z0-9][a-z0-9-]{0,79}', data['id']):
            raise InstallerError('Invalid profile id')
        positive_int(data['revision'], 'profile revision')
        if data['kind'] not in ('production', 'synthetic') or data['approval'] not in ('reviewed', 'unconfigured'):
            raise InstallerError('Invalid profile kind or approval')
        identity = data['identity']
        keys(identity, {'label', 'provenance', 'inputs'})
        string(identity['label'], 'identity label')
        string(identity['provenance'], 'identity provenance')
        if not isinstance(identity['inputs'], list):
            raise InstallerError('Input identities must be a list')
        seen = set()
        for item in identity['inputs']:
            keys(item, {'path', 'size', 'sha256'})
            relative_path(item['path'])
            positive_int(item['size'], 'input size', zero=True)
            sha256(item['sha256'])
            folded = item['path'].casefold()
            if folded in seen:
                raise InstallerError('Duplicate input path')
            seen.add(folded)
        if not isinstance(data['platforms'], dict) or not data['platforms'] or set(data['platforms']) - HOSTS:
            raise InstallerError('Unknown or missing platform adapters')
        for host, adapter in data['platforms'].items():
            keys(adapter, {'status', 'reason', 'tools', 'stages', 'package'})
            if adapter['status'] not in ('ready', 'blocked'):
                raise InstallerError('Unknown platform readiness')
            string(adapter['reason'], 'platform reason')
            for field in ('tools', 'stages', 'package'):
                if not isinstance(adapter[field], list):
                    raise InstallerError(f'{field} must be a list')
            if adapter['status'] == 'ready' and (data['approval'] != 'reviewed' or not identity['inputs']):
                raise InstallerError('A ready recipe requires reviewed input identity')
            tool_ids = set()
            for tool in adapter['tools']:
                keys(tool, {'id', 'version', 'url', 'sha256', 'size', 'format', 'max_expanded_size', 'max_entries', 'executable', 'license'})
                if not isinstance(tool['id'], str) or not re.fullmatch('[a-z0-9][a-z0-9-]{0,79}', tool['id']) or tool['id'] in tool_ids:
                    raise InstallerError('Invalid or duplicate tool id')
                tool_ids.add(tool['id'])
                for field in ('version', 'url', 'license'): string(tool[field], f'tool {field}')
                sha256(tool['sha256'])
                for field in ('size', 'max_expanded_size', 'max_entries'): positive_int(tool[field], field)
                if tool['format'] not in ('zip', 'tar.gz'): raise InstallerError('Unsupported archive format')
                relative_path(tool['executable'])
            stage_names = []
            for stage in adapter['stages']:
                keys(stage, {'name', 'tool', 'argv', 'outputs'})
                if stage['name'] not in ('generate', 'build', 'package') or stage['tool'] not in tool_ids:
                    raise InstallerError('Unknown stage or tool')
                stage_names.append(stage['name'])
                if not isinstance(stage['argv'], list) or any(not isinstance(a, str) or '\x00' in a for a in stage['argv']):
                    raise InstallerError('Command arguments must be an array of strings')
                if not isinstance(stage['outputs'], list): raise InstallerError('Stage outputs must be a list')
                for output in stage['outputs']: relative_path(output)
            if stage_names and stage_names != ['generate', 'build', 'package']:
                raise InstallerError('Recipes require generate, build, package in order')
            if data['kind'] == 'production' and adapter['status'] == 'ready' and not stage_names:
                raise InstallerError('Production recipe is not configured')
            for entry in adapter['package']: relative_path(entry)
            if data['kind'] == 'production' and adapter['status'] == 'ready' and not adapter['package']:
                raise InstallerError('Application package allowlist is empty')
        digest = hashlib.sha256(json.dumps(data, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        return cls(data, digest)

    def adapter(self, host=None):
        host = host or host_key()
        if host not in self.data['platforms']:
            raise InstallerError(f'No recipe for host {host}; foreign-platform builds are disabled')
        adapter = self.data['platforms'][host]
        if adapter['status'] != 'ready':
            raise InstallerError(f'{host} is blocked: {adapter["reason"]}')
        return adapter
