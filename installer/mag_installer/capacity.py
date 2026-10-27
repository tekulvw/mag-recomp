"""Read-only known storage bounds; build scratch requirements remain unknown."""
from pathlib import Path
import shutil
from .model import InstallerError, host_key


def volume(path):
    existing = Path(path)
    while not existing.exists():
        existing = existing.parent
    return existing.stat().st_dev, shutil.disk_usage(existing).free


def estimate(profile, source, output, cache):
    adapter = profile.adapter(host_key())
    groups = {}
    def reserve(path, amount):
        device, free = volume(path)
        group = groups.setdefault(device, {'path': str(path), 'required_bytes': 0, 'free_bytes': free})
        group['free_bytes'] = min(group['free_bytes'], free)
        group['required_bytes'] += amount
    snapshot = sum(item['size'] for item in profile.data['identity']['inputs'])
    expanded = sum(tool['max_expanded_size'] for tool in adapter['tools'])
    # TAR extraction additionally keeps its bounded decompressed archive alive.
    tar_scratch = max((tool['max_expanded_size'] + tool['max_entries'] * 4096 + 10240
                       for tool in adapter['tools'] if tool['format'] == 'tar.gz'), default=0)
    reserve(Path(output).parent, snapshot + expanded + tar_scratch)
    downloads = 0
    seen = set()
    for tool in adapter['tools']:
        if tool['sha256'] in seen: continue
        seen.add(tool['sha256'])
        blob = Path(cache) / f'{tool["sha256"]}.blob'
        # A cache hit is reverified by acquisition; a bad hit fails before writing.
        if not blob.is_file(): downloads += tool['size']
    reserve(Path(cache), downloads)
    for group in groups.values():
        if group['required_bytes'] > group['free_bytes']:
            raise InstallerError(f'Insufficient free space at {group["path"]}: known requirement '
                                 f'{group["required_bytes"]} bytes, available {group["free_bytes"]} bytes; '
                                 'additional build working space is unknown')
    return {'download_bytes': downloads, 'required_bytes': sum(g['required_bytes'] for g in groups.values()),
            'free_bytes': sum(g['free_bytes'] for g in groups.values()), 'estimate_complete': False,
            'filesystems': list(groups.values())}
