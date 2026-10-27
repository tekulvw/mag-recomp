"""Bind the built runner to its generated image and reviewed framework revision."""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path
if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.prepare_inputs import ROOT, sha256_file, verify_hash, data_path, load_framework


def validate_lift(directory: Path, elf_hash: str) -> dict:
    record=json.loads((directory/'provenance.json').read_text())
    if record.get('status') != 'complete' or record.get('exit_code') != 0 or record.get('audit_exit') != 0:
        raise ValueError('Lift generation and audit must both have completed successfully')
    if record['elf_sha256'] != elf_hash or not record.get('outputs'):
        raise ValueError('Lift identity/output hashes are missing or do not match the selected ELF')
    files={p.name for pattern in ('*.c','*.cpp','*.h') for p in directory.glob(pattern)}
    if files != set(record['outputs']):
        raise ValueError('Generated source set has changed since lifting')
    for name, digest in record['outputs'].items():
        if Path(name).name != name:
            raise ValueError('Generated output name must be a basename')
        verify_hash(directory/name, digest)
    return record


def validate_receipt(binary: Path, elf_hash: str) -> dict:
    record=json.loads(binary.with_suffix('.receipt.json').read_text())
    if record['elf_sha256'] != elf_hash:
        raise ValueError('Native binary was built for a different ELF')
    verify_hash(binary, record['binary_sha256'])
    return record


def validate_generated(directory: Path, profile: dict, kind: str) -> dict:
    record=json.loads((directory/'provenance.json').read_text())
    if record.get('status') != 'complete':
        raise ValueError('Generated module must be complete')
    if kind == 'spu':
        count=record.get('unsupported_count')
        if type(count) is not int or count < 0:
            raise ValueError('SPU generation must report its unsupported count')
        if profile.get('executed_todo_policy') == 'halt':
            if record.get('executed_todo_policy') != 'halt' or record.get('guarded_todo_count') != count:
                raise ValueError('Engine SPU unsupported sites must all halt')
            if profile.get('expected_unsupported_count') != count:
                raise ValueError('Engine SPU unsupported census differs from its reviewed profile')
            if 'training_task' in profile:
                image_counts=[]
                for key,name in [('policy','policy_unsupported_count'),('training_task','training_unsupported_count'),
                                 ('queued_training_task','queued_training_unsupported_count')]:
                    if key not in profile:continue
                    value=record.get(name)
                    if type(value) is not int or value<0 or value!=profile[key]['expected_unsupported_count']:
                        raise ValueError('Engine SPU image unsupported census differs: '+key)
                    image_counts.append(value)
                if sum(image_counts)!=count:
                    raise ValueError('Engine SPU image censuses do not match the aggregate')
        elif count != 0:
            raise ValueError('SPU generation must complete without unsupported code')
    if kind == 'firmware' and record.get('executed_todo_policy') != 'halt':
        raise ValueError('Firmware lift must stop at executed unsupported code')
    if record.get('profile') != profile or not record.get('outputs'):
        raise ValueError('Generated image/profile identity does not match this build')
    files={str(p.relative_to(directory)) for p in directory.rglob('*')
           if p.is_file() and p.suffix in ('.c','.h','.cpp')}
    if files != set(record['outputs']):
        raise ValueError('Generated source set has changed')
    for name,digest in record['outputs'].items():
        path=directory/name
        if path.is_symlink() or not path.resolve().is_relative_to(directory.resolve()):
            raise ValueError('Generated source must stay inside its output directory')
        verify_hash(path,digest)
    return record


def validate_spu(directory: Path, profile: dict) -> dict:
    return validate_generated(directory,profile,'spu')


def validate_modules(image: dict, image_name: str, spu: Path | None,
                     firmware: Path | None) -> dict:
    result={'launcher_network_hooks': image_name == 'EBOOT' and firmware is not None}
    if spu is None and firmware is None:
        return result
    if image_name == 'mage_g':
        review_path=ROOT/'profiles/retail-v1.0-engine-modules.json'
        if not review_path.is_file():
            raise ValueError('Engine module review is required')
        review=json.loads(review_path.read_text())
        if review.get('image_name') != image_name or review.get('elf_sha256') != image['elf_sha256']:
            raise ValueError('Engine module review does not match selected image')
        # A matching dispatch fingerprint is insufficient: bind every embedded
        # SPU byte to the already audited worker and the selected full ELF hash.
        profile=json.loads((ROOT/'profiles/retail-v1.0-spu.json').read_text())
        worker=data_path(profile['image']);verify_hash(worker,profile['image_sha256'])
        embedded=review['embedded_spu']
        if (embedded['sha256'] != profile['image_sha256'] or
                embedded['size'] != profile['image_bytes'] or
                embedded['offset'] < 0 or worker.stat().st_size != embedded['size']):
            raise ValueError('Engine embedded SPU identity/size does not match audited worker')
        with Path(image['elf']).open('rb') as stream:
            stream.seek(embedded['offset']); data=stream.read(embedded['size'])
        if len(data) != embedded['size'] or hashlib.sha256(data).hexdigest() != embedded['sha256']:
            raise ValueError('Engine embedded SPU bytes do not match audited worker')
        result['module_review']=review
    for kind,directory in [('spu',spu),('firmware',firmware)]:
        if directory is None:
            continue
        profile_path=ROOT/f'profiles/retail-v1.0-{kind}.json'
        if image_name == 'mage_g':
            specific=review.get('image_profiles',{}).get(kind)
            if specific:
                expected=f'profiles/retail-v1.0-engine-{kind}.json'
                if specific.get('path') != expected:
                    raise ValueError('Engine module profile path is not the reviewed image profile')
                profile_path=ROOT/expected
                if specific.get('sha256') != sha256_file(profile_path):
                    raise ValueError('Engine module review does not match image-specific '+kind+' profile')
            elif review.get('shared_profiles',{}).get(kind) != sha256_file(profile_path):
                raise ValueError('Engine module review does not match shared '+kind+' profile')
        profile=json.loads(profile_path.read_text())
        if image_name == 'mage_g' and review.get('image_profiles',{}).get(kind):
            if profile.get('image_name') != image_name or profile.get('elf_sha256') != image['elf_sha256']:
                raise ValueError('Engine module profile does not match the selected ELF')
        result[kind+'_provenance']=validate_generated(directory,profile,kind)
    return result


def validate_build_image(recomp: Path, image_name: str = 'EBOOT',
                         firmware: Path | None = None,
                         spu: Path | None = None) -> tuple[dict, dict]:
    if image_name not in ('EBOOT', 'mage_g'):
        raise ValueError('Image must be EBOOT or mage_g')
    image=json.loads((data_path('profiles/retail-v1.0.json')).read_text())['images'][image_name]
    verify_hash(Path(image['elf']), image['elf_sha256'])
    lift=validate_lift(recomp, image['elf_sha256'])
    if firmware is not None or spu is not None:
        validate_modules(image,image_name,spu,firmware)
    return image,lift


def record_build(binary: Path, sdk: Path, recomp: Path, spu: Path | None = None,
                 firmware: Path | None = None, image_name: str = 'EBOOT') -> dict:
    image,lift=validate_build_image(recomp,image_name)
    revision=subprocess.check_output(['git','-C',str(sdk),'rev-parse','HEAD'],text=True).strip()
    dirty=subprocess.check_output(['git','-C',str(sdk),'status','--porcelain','--untracked-files=no'],text=True)
    allowed=load_framework()[0]['allowed_runtime_revisions']
    if revision not in allowed or dirty:
        raise ValueError('Runtime SDK must be clean and recorded in profiles/framework.json')
    record=dict(binary_sha256=sha256_file(binary), image_name=image_name, elf_sha256=image['elf_sha256'],
                sdk_revision=revision, sdk_path=str(sdk), lift_provenance=lift,
                runner_sources={str(p.relative_to(ROOT)):sha256_file(p) for p in
                                [ROOT/'CMakeLists.txt', *sorted((ROOT/'src').glob('*'))] if p.is_file()})
    record.update(validate_modules(image,image_name,spu,firmware))
    binary.with_suffix('.receipt.json').write_text(json.dumps(record,indent=2)+'\n')
    return record


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path)
    parser.add_argument('--sdk',type=Path)
    parser.add_argument('--recomp',type=Path,required=True)
    parser.add_argument('--image',choices=['EBOOT','mage_g'],default='EBOOT')
    parser.add_argument('--validate-image',action='store_true',help='Validate the selected ELF and lift without creating a receipt')
    parser.add_argument('--spu',type=Path)
    parser.add_argument('--firmware',type=Path)
    args=parser.parse_args()
    if args.validate_image:
        validate_build_image(args.recomp,args.image,args.firmware,args.spu)
    else:
        if args.binary is None or args.sdk is None:
            parser.error('--binary and --sdk are required when recording a build')
        record_build(args.binary,args.sdk,args.recomp,args.spu,args.firmware,args.image)
