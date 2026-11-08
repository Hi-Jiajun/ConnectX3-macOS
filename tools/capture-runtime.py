#!/usr/bin/env python3
"""Collect one identity-bracketed PCIe or RSS observation against a candidate.

Requires explicit mode-specific opt-in. RSS selector 35 changes MAC/QP state.
Never installs, reloads, resets, retries a
device operation, or accepts arbitrary commands. Output is a new evidence folder.
Identity/hash matching does not certify DMA safety or physical provenance.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import uuid

_spec = importlib.util.spec_from_file_location('cx3_runtime_decoder', Path(__file__).with_name('decode-runtime.py'))
decoder = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(decoder)


def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def image_uuid(path):
    data = Path(path).read_bytes()
    if len(data) < 32: raise ValueError('candidate Mach-O header missing')
    magic, cpu, _, _, count, size, _, _ = struct.unpack_from('<IiiIIIII', data)
    if magic != 0xfeedfacf or cpu != 0x1000007 or count > 4096 or size > len(data)-32:
        raise ValueError('expected bounded Intel Mach-O64 load commands')
    end, cursor, values = 32+size, 32, []
    for _ in range(count):
        if cursor+8 > end: raise ValueError('truncated Mach-O load command')
        cmd, length = struct.unpack_from('<II', data, cursor)
        if length < 8 or length & 7 or cursor+length > end: raise ValueError('invalid Mach-O command size')
        if cmd == 0x1b:
            if length != 24: raise ValueError('invalid LC_UUID size')
            values.append(str(uuid.UUID(bytes=data[cursor+8:cursor+24])).upper())
        cursor += length
    if cursor != end or len(values) != 1 or not uuid.UUID(values[0]).int:
        raise ValueError('candidate requires exactly one nonzero LC_UUID')
    return values[0]


def candidate(manifest):
    document = decoder.read_document(manifest)
    if not isinstance(document, dict): raise ValueError('candidate manifest must be an object')
    identity = document.get('identity', {})
    if not isinstance(identity, dict): raise ValueError('candidate identity must be an object')
    if document.get('passed') is not True or identity.get('passed') is not True:
        raise ValueError('candidate verification did not pass')
    if not isinstance(document.get('buildTag'), str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+\-]{0,63}', document['buildTag']) or identity.get('buildTag') != document['buildTag']:
        raise ValueError('candidate build metadata mismatch')
    version = identity.get('bundleVersion')
    if not isinstance(version, str) or not re.fullmatch(r'[0-9]+(?:\.[0-9]+){0,2}', version) or identity.get('version') != version:
        raise ValueError('candidate release metadata mismatch')
    root = manifest.parent.resolve(); artifacts = document.get('artifacts')
    if not isinstance(artifacts, list) or len(artifacts) != 2:
        raise ValueError('candidate requires exact driver and CLI artifact records')
    paths = {}
    for record in artifacts:
        if not isinstance(record, dict) or not isinstance(record.get('path'), str): raise ValueError('invalid artifact record')
        relative = Path(record['path'])
        if relative.is_absolute() or '..' in relative.parts: raise ValueError('artifact path must stay in candidate folder')
        path = (root/relative).resolve()
        try: path.relative_to(root)
        except ValueError as e: raise ValueError('artifact resolves outside candidate folder') from e
        expected = record.get('sha256')
        if not isinstance(expected, str) or not re.fullmatch(r'[0-9a-f]{64}', expected) or sha(path) != expected:
            raise ValueError('candidate artifact SHA-256 mismatch')
        kind = 'driver' if str(relative).endswith('/Cx3Mlx4.kext/Contents/MacOS/Cx3Mlx4') else 'cli' if relative.name == 'cx3ctl' else None
        if not kind or kind in paths: raise ValueError('unexpected or duplicate candidate artifact')
        paths[kind] = path
    if set(paths) != {'driver', 'cli'}: raise ValueError('missing driver or CLI artifact')
    return document, paths, image_uuid(paths['driver'])


def loaded_identity(text):
    pattern = r'\bcom\.hijiajun\.driver\.Cx3Mlx4\s+\(([^)\s]+)\)\s+([0-9A-Fa-f-]{36})(?:\s|$)'
    matches = re.findall(pattern, text)
    if len(matches) != 1: raise ValueError('OS listing must identify exactly one loaded CX3 kext')
    release, value = matches[0]
    if not re.fullmatch(r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}', value) or not uuid.UUID(value).int:
        raise ValueError('OS loaded UUID is invalid')
    return release, value.upper()


def boot_uuid(text):
    value = text.strip()
    if not re.fullmatch(r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}', value) or not uuid.UUID(value).int:
        raise ValueError('OS boot UUID is unavailable')
    return value.upper()


def capture(manifest, output, *, allow_pci_read=False, observation='pcie',
            allow_rss_experiment=False, runner=subprocess.run):
    if observation not in ('pcie','rss'): raise ValueError('unsupported observation')
    if observation=='pcie' and not allow_pci_read: raise ValueError('explicit PCI read opt-in is required')
    if observation=='rss' and not allow_rss_experiment: raise ValueError('explicit RSS experiment opt-in is required')
    stage = 'boundpcie' if observation=='pcie' else 'boundrss'
    manifest, output = Path(manifest).resolve(), Path(output).resolve()
    output.mkdir(exist_ok=False, parents=False)
    receipt = dict(schema='cx3-runtime-receipt-v1', validated=False, commands=[],
                   observation=observation,
                   started_utc=datetime.now(timezone.utc).isoformat(),
                   started_monotonic_ns=str(time.monotonic_ns()),
                   dma_quiescence_proven=False, atomic_snapshot=False,
                   scope='candidate/OS/endpoint identity and byte association; no feature or stability certification')

    def run(name, cmd):
        event = dict(stage=name, argv=cmd, exit_code=None)
        receipt['commands'].append(event)

        def save_stream(suffix, value):
            if isinstance(value, bytes): value = value.decode('utf-8', errors='replace')
            path = output/f'{name}.{suffix}'
            path.write_text(value or '')
            event[f'{suffix}_sha256'] = sha(path)
            if name == stage and suffix == 'stdout':
                receipt['capture_file_sha256'] = event['stdout_sha256']

        try:
            result = runner(cmd, capture_output=True, text=True, errors='replace', timeout=30, check=False)
        except subprocess.TimeoutExpired as error:
            event['timed_out'] = True
            event['timeout_cancels_device_operation'] = False
            for suffix, value in (('stdout', error.stdout), ('stderr', error.stderr)):
                save_stream(suffix, value)
            raise
        save_stream('stdout', result.stdout)
        save_stream('stderr', result.stderr)
        event['exit_code'] = result.returncode
        return result

    def probe(name, cmd, parser):
        result = run(name, cmd)
        if result.returncode: raise ValueError(f'{name} read-only OS probe failed: {result.returncode}')
        return parser(result.stdout)

    try:
        manifest_sha = sha(manifest)
        meta, paths, expected_uuid = candidate(manifest)
        receipt.update(candidate_manifest=str(manifest), candidate_manifest_sha256=manifest_sha,
                       build_tag=meta['buildTag'], expected_uuid=expected_uuid)
        expected_hashes = {kind:sha(path) for kind,path in paths.items()}
        boot_before = probe('boot-before', ['/usr/sbin/sysctl','-n','kern.bootsessionuuid'], boot_uuid)
        loaded_before = probe('loaded-before', ['/usr/bin/kmutil','showloaded','--list-only'], loaded_identity)
        expected_loaded = (meta['identity']['bundleVersion'], expected_uuid)
        if loaded_before != expected_loaded: raise ValueError('loaded image differs from frozen candidate; no selector invoked')
        command = [str(paths['cli']), stage]
        if observation=='rss': command.append('--allow-rss-experiment')
        result = run(stage, command)
        # OS-only observations continue after CLI failure; never repeat the selector.
        boot_after = probe('boot-after', ['/usr/sbin/sysctl','-n','kern.bootsessionuuid'], boot_uuid)
        loaded_after = probe('loaded-after', ['/usr/bin/kmutil','showloaded','--list-only'], loaded_identity)
        if sha(manifest) != manifest_sha or any(sha(path) != expected_hashes[kind] for kind,path in paths.items()):
            raise ValueError('candidate manifest/binary bytes changed during collection')
        document = decoder.read_document(output/f'{stage}.stdout')
        decoded = decoder.decode_bound(document) if observation=='pcie' else decoder.decode_bound_rss(document)
        (output/'decoded.json').write_text(json.dumps(decoded,indent=2)+'\n')
        receipt['capture_canonical_sha256'] = decoded['capture_sha256']
        receipt['identity_binding_valid'] = decoded['binding_valid']
        if observation=='rss': receipt['operation_succeeded'] = decoded['operation_succeeded']
        if result.returncode or not decoded['binding_valid']: raise ValueError('CLI or independent capture validation failed')
        if observation=='rss' and not decoded['operation_succeeded']:
            raise ValueError('RSS diagnostic is bound but the device operation did not succeed')
        if boot_before != boot_after or loaded_before != loaded_after:
            raise ValueError('OS boot or loaded image changed during collection')
        for side in ('before', 'after'):
            identity = decoded[side]
            if identity['boot_session_uuid'] != boot_before or identity['build_tag'] != meta['buildTag'] or identity['bundle_version'] != meta['identity']['bundleVersion']:
                raise ValueError('endpoint/OS/candidate identity mismatch')
        receipt.update(validated=True, loaded_uuid=expected_uuid, boot_session_uuid=boot_before,
                       artifact_sha256=expected_hashes,
                       service_registry_id=decoded['before']['service_registry_id'],
                       provider_registry_id=decoded['before']['provider_registry_id'],
                       device_generation=decoded['before']['device_generation'])
    except (OSError, ValueError, KeyError, TypeError, struct.error, subprocess.TimeoutExpired) as error:
        receipt['error'] = str(error)
    receipt['finished_monotonic_ns'] = str(time.monotonic_ns())
    (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    return receipt['validated']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, required=True, help='frozen final/verification.json')
    parser.add_argument('--output', type=Path, required=True, help='new evidence directory; parent must exist')
    parser.add_argument('--allow-pci-read', action='store_true', help='permit the single existing PCIe config query, after matching the loaded candidate')
    parser.add_argument('--observation',choices=('pcie','rss'),default='pcie')
    parser.add_argument('--allow-rss-experiment',action='store_true',help='permit one existing state-changing selector-35 experiment, after matching the loaded candidate')
    args = parser.parse_args()
    if args.observation=='pcie' and not args.allow_pci_read: parser.error('--allow-pci-read is required for PCIe')
    if args.observation=='rss' and not args.allow_rss_experiment: parser.error('--allow-rss-experiment is required for RSS')
    try: ok = capture(args.candidate,args.output,allow_pci_read=args.allow_pci_read,
                      observation=args.observation,allow_rss_experiment=args.allow_rss_experiment)
    except (OSError, ValueError) as e:
        print(f'capture-runtime: {e}',file=sys.stderr); return 1
    if not ok: print(f'capture-runtime: validation failed; evidence retained in {args.output}',file=sys.stderr)
    return 0 if ok else 1


if __name__=='__main__': raise SystemExit(main())
