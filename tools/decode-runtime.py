#!/usr/bin/env python3
"""Independently validate saved host runtime identities and bracketed PCIe bytes.

No IOKit/device/network access. Boot/service identifies an IOService instance;
generation is bookkeeping. Neither identity nor a payload hash proves DMA safety.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import sys
import uuid


def uint(document, key, bits=32):
    value = document.get(key)
    if type(value) is not int or not 0 <= value < 1 << bits:
        raise ValueError(f'{key} must be an unsigned {bits}-bit integer')
    return value


def encoded_bytes(document, key, size):
    value = document.get(key)
    if not isinstance(value, str) or not re.fullmatch(r'[0-9a-fA-F]{%d}' % (size*2), value):
        raise ValueError(f'{key} must encode exactly {size} bytes')
    return bytes.fromhex(value)


def cstring(raw, offset, size, pattern):
    value = raw[offset:offset+size]
    if b'\0' not in value:
        raise ValueError('unterminated identity string')
    text, tail = value.split(b'\0', 1)
    if any(tail): raise ValueError('nonzero string padding')
    try: text = text.decode('ascii')
    except UnicodeDecodeError as e: raise ValueError('non-ASCII identity metadata') from e
    if text and not re.fullmatch(pattern, text): raise ValueError('malformed identity metadata')
    return text


def h64(value): return f'0x{value:016x}'


def decode_identity(document):
    if not isinstance(document, dict) or document.get('schema') != 'cx3-runtime-identity-v1':
        raise ValueError('unsupported runtime identity schema')
    boot = document.get('bootSessionUuid')
    if not isinstance(boot, str) or not re.fullmatch(r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}', boot) or not uuid.UUID(boot).int:
        raise ValueError('missing/invalid boot-session UUID')
    connected = document.get('connectedServiceRegistryId')
    if not isinstance(connected, str) or not re.fullmatch(r'0x[0-9a-fA-F]{16}', connected):
        raise ValueError('connected service ID must be a lossless 64-bit hex string')
    transport = uint(document, 'transport')
    size, scalars = uint(document, 'returnedBytes', 64), uint(document, 'scalarOutputCount')
    result = dict(complete=False, boot_session_uuid=boot.upper(),
                  connected_service_registry_id=h64(int(connected, 16)),
                  transport=transport, dma_quiescence_proven=False,
                  identity_scope='IOService object within one OS boot', issues=[])
    if transport:
        result['issues'] = ['identity transport failed']; return result
    if size != 176 or scalars != 0: raise ValueError('runtime response dimensions mismatch')
    raw = encoded_bytes(document, 'rawHex', 176)
    version, length, status, validity, service, provider, uptime, generation, flags, reserved = struct.unpack_from('<II6QII', raw)
    if version != 1 or length != 176 or validity & ~63 or flags & ~31:
        raise ValueError('unsupported runtime ABI/validity/lifecycle flags')
    pci = struct.unpack_from('<4H4B', raw, 64)
    if reserved or pci[-1] or any(raw[173:]) or pci[5] >= 32 or pci[6] >= 8:
        raise ValueError('invalid PCI identity or nonzero reserved bytes')
    release = cstring(raw, 76, 32, r'[0-9]+(?:\.[0-9]+){0,2}|unknown')
    tag = cstring(raw, 108, 65, r'[A-Za-z0-9][A-Za-z0-9._+\-]{0,63}')
    if int(connected, 16) != service: raise ValueError('connected service differs from exported service')
    issues = result['issues']
    if status: issues.append('identity payload reports failure')
    if validity != 63: issues.append('required metadata validity missing')
    if not all((service, provider, uptime, generation)): issues.append('required identity value missing')
    if any(x in (0, 0xffff) for x in pci[:4]): issues.append('PCI snapshot is unavailable')
    if not release or release == 'unknown' or not tag or tag in ('dev', 'unversioned-local', 'unknown'):
        issues.append('candidate build metadata is unavailable')
    result.update(complete=not issues, status=h64(status), validity=validity,
                  service_registry_id=h64(service), provider_registry_id=h64(provider),
                  sample_uptime_ns=h64(uptime), device_generation=h64(generation),
                  lifecycle_flags=flags, pci_identity=list(pci[:-1]),
                  bundle_version=release, build_tag=tag,
                  payload_sha256=hashlib.sha256(raw).hexdigest())
    return result


def decode_bound(document):
    if not isinstance(document, dict) or document.get('schema') != 'cx3-bound-pcie-v1':
        raise ValueError('unsupported bound PCIe schema')
    if type(document.get('identityBracketMatched')) is not bool:
        raise ValueError('identityBracketMatched must be a boolean')
    before, after = decode_identity(document.get('before')), decode_identity(document.get('after'))
    issues, decoded = [], None
    if not before['complete'] or not after['complete']:
        issues.append('identity metadata incomplete')
    else:
        for key in ('boot_session_uuid', 'connected_service_registry_id',
                    'service_registry_id', 'provider_registry_id', 'device_generation',
                    'pci_identity', 'bundle_version', 'build_tag', 'lifecycle_flags'):
            if before[key] != after[key]: issues.append(f'{key} changed')
        if int(after['sample_uptime_ns'], 16) < int(before['sample_uptime_ns'], 16):
            issues.append('sample clock moved backward')
        if (before['lifecycle_flags'] | after['lifecycle_flags']) & 15:
            issues.append('lifecycle refuses device sampling')
    pcie = document.get('pcie')
    if not isinstance(pcie, dict): raise ValueError('missing PCIe observation')
    if uint(pcie, 'transport'):
        issues.append('PCIe transport failed or was not attempted')
    else:
        try:
            if uint(pcie, 'returnedBytes', 64) != 368: raise ValueError('PCIe response length mismatch')
            raw = encoded_bytes(pcie, 'rawHex', 368)
            spec = importlib.util.spec_from_file_location('cx3_independent_pcie', Path(__file__).with_name('decode-pcie.py'))
            module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
            decoded = module.decode_capture(dict(selector=48, call=0, inCount=0, outCount=0,
                **{'in': [], 'out': [], 'structLen':368, 'structHex':raw.hex()}))
            if decoded['capture_state'] != 'captured': issues.append('PCIe query incomplete')
            if before['complete']:
                if h64(struct.unpack_from('<Q', raw, 24)[0]) != before['device_generation']:
                    issues.append('PCIe generation differs from identity')
                if list(struct.unpack_from('<4H4B', raw, 56)[:-1]) != before['pci_identity'] or raw[56:68] != raw[68:80]:
                    issues.append('PCIe sample identity differs from cached start identity')
        except (ValueError, KeyError, TypeError) as e:
            issues.append(f'PCIe bytes rejected: {e}')
    if not document['identityBracketMatched']: issues.append('producer did not establish identity bracket')
    return dict(schema='cx3-validated-bound-pcie-v1', binding_valid=not issues,
                before=before, after=after, pcie=decoded, issues=issues,
                capture_sha256=hashlib.sha256(json.dumps(document, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()).hexdigest(),
                atomic_snapshot=False, dma_quiescence_proven=False,
                hardware_provenance='unverified saved input; identity/hash bind observations only')


def decode_bound_rss(document):
    """Bind one selector-35 result to a non-atomic same-instance bracket.

    Device-level failure remains inspectable and distinct from byte binding.
    This result ABI carries no identity itself; the bracket associates saved
    observations, not an exclusive transaction or authenticated provenance.
    """
    if not isinstance(document, dict) or document.get('schema') != 'cx3-bound-rss-v1':
        raise ValueError('unsupported bound RSS schema')
    if type(document.get('identityBracketMatched')) is not bool:
        raise ValueError('identityBracketMatched must be a boolean')
    before, after = decode_identity(document.get('before')), decode_identity(document.get('after'))
    issues, decoded = [], None
    if not before['complete'] or not after['complete']:
        issues.append('identity metadata incomplete')
    else:
        for key in ('boot_session_uuid', 'connected_service_registry_id',
                    'service_registry_id', 'provider_registry_id', 'device_generation',
                    'pci_identity', 'bundle_version', 'build_tag', 'lifecycle_flags'):
            if before[key] != after[key]: issues.append(f'{key} changed')
        if int(after['sample_uptime_ns'],16) < int(before['sample_uptime_ns'],16):
            issues.append('sample clock moved backward')
        if (before['lifecycle_flags'] | after['lifecycle_flags']) & 15:
            issues.append('lifecycle refuses RSS experiment')
    observation = document.get('rss')
    try:
        if not isinstance(observation,dict): raise ValueError('missing RSS observation')
        if uint(observation,'selector') != 35 or uint(observation,'call') != 0:
            raise ValueError('RSS selector/transport failed or was not attempted')
        if uint(observation,'inCount') != 0 or observation.get('in') != []:
            raise ValueError('RSS accepts no scalar inputs')
        if uint(observation,'outCount') != 15 or not isinstance(observation.get('out'),list) or len(observation['out']) != 15:
            raise ValueError('RSS scalar output dimensions mismatch')
        for value in observation['out']:
            if not isinstance(value,str) or not re.fullmatch(r'0x[0-9a-fA-F]{1,16}',value):
                raise ValueError('RSS scalar output must be lossless uint64 hex')
        spec = importlib.util.spec_from_file_location('cx3_bound_qpc',Path(__file__).with_name('decode-qpc.py'))
        qpc = importlib.util.module_from_spec(spec);spec.loader.exec_module(qpc)
        decoded = qpc.decode_result(observation)
        if int(observation['out'][0],16) != decoded['result_status'] or int(observation['out'][5],16) != decoded['qpn']:
            raise ValueError('RSS scalar status/QPN disagrees with result bytes')
        if before['complete'] and after['complete']:
            low,high = int(before['sample_uptime_ns'],16),int(after['sample_uptime_ns'],16)
            for entry in decoded['entries']:
                command = entry['observation']['command']
                if command['flags'] and not low <= command['start_ns'] <= command['end_ns'] <= high:
                    issues.append(f"{entry['label']} command lies outside identity bracket")
    except (ValueError,KeyError,TypeError) as error:
        issues.append(f'RSS bytes rejected: {error}')
    if not document['identityBracketMatched']: issues.append('producer did not establish identity bracket')
    # status=0 can mean only "selector answered; device refused" (the real
    # first qpModify failure path). A fresh successful final query is required.
    final = decoded['entries'][-1] if decoded else None
    completed = bool(not issues and decoded and decoded['result_status']==0 and final and
                     final['observation']['captured_bytes']==4096 and
                     final['observation']['command']['flags']==7 and
                     final['observation']['fw_status']==0 and final['decoded'] and
                     final['decoded']['state']==3 and final['decoded']['local_qpn']==decoded['qpn'] and
                     0 < decoded['qpn'] < (1 << 24) and
                     int(observation['out'][8],16)==1 and int(observation['out'][9],16)==3)
    return dict(schema='cx3-validated-bound-rss-v1',binding_valid=not issues,
                operation_succeeded=completed,
                before=before,after=after,rss=decoded,issues=issues,
                capture_sha256=hashlib.sha256(json.dumps(document,sort_keys=True,separators=(',', ':'),ensure_ascii=True).encode()).hexdigest(),
                atomic_snapshot=False,dma_quiescence_proven=False,
                hardware_provenance='unverified saved input; identity/hash bracket observations only')


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result: raise ValueError(f'duplicate JSON key: {key}')
        result[key] = value
    return result


def read_document(path):
    return json.loads(Path(path).read_text(), object_pairs_hook=unique_object,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError(f'invalid JSON constant: {value}')))


def main():
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('capture')
    args = parser.parse_args()
    try:
        document = read_document(args.capture)
        if not isinstance(document, dict): raise ValueError('capture must be a JSON object')
        if document.get('schema') == 'cx3-bound-pcie-v1': decoded = decode_bound(document)
        elif document.get('schema') == 'cx3-bound-rss-v1': decoded = decode_bound_rss(document)
        else: decoded = decode_identity(document)
        print(json.dumps(decoded, indent=2))
        return 0 if decoded.get('binding_valid', decoded.get('complete', False)) else 1
    except (OSError, ValueError, TypeError, KeyError) as e:
        print(f'decode-runtime: {e}', file=sys.stderr); return 1


if __name__ == '__main__': raise SystemExit(main())
