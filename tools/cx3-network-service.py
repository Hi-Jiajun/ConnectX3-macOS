#!/usr/bin/python3
"""Opt-in one-shot network service. No self-install, retry, reset or reboot.
Default is a dry run. Start with enabled=false does not even open the driver.
"""
import argparse,json,re,struct,subprocess,sys
from pathlib import Path

class Stop(RuntimeError): pass

def load_config(path):
    c=json.loads(Path(path).read_text())
    if not isinstance(c,dict) or set(c)!={'schema','enabled','dhcp','cli','expectedBuild'}:
        raise Stop('config requires schema, enabled, dhcp, cli, expectedBuild only')
    if c['schema']!='cx3-network-service-v1' or type(c['enabled']) is not bool or type(c['dhcp']) is not bool:
        raise Stop('bad schema or boolean')
    if not isinstance(c['cli'],str) or not Path(c['cli']).is_absolute(): raise Stop('cli must be absolute')
    if not isinstance(c['expectedBuild'],str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+\-]{0,63}',c['expectedBuild']):
        raise Stop('expectedBuild must be explicit')
    return c

def decode_control(envelope,action):
    if not isinstance(envelope,dict) or envelope.get('schema')!='cx3-auto-network-v1' or envelope.get('ok') is not True or envelope.get('bound') is not True:
        raise Stop('CLI refused or identity binding failed')
    control=envelope.get('control',{})
    if control.get('transport')!=0 or control.get('returnedBytes')!=208: raise Stop('bad control transport/size')
    try: raw=bytes.fromhex(control['rawHex']);v=struct.unpack('<IIQ8I3QII16Q',raw)
    except (ValueError,KeyError,struct.error,TypeError) as e: raise Stop('malformed control result') from e
    version,size,status,state,returned_action,stage,completed,failed,selector,unit,port,generation,mask,last_status,flags,reserved,*scalars=v
    if version!=1 or size!=208 or status or returned_action!=action or port!=1 or not generation or reserved:
        raise Stop('control request/result mismatch')
    if action==1 and (state!=2 or mask!=0xffff or failed or flags&28): raise Stop('network did not initialize')
    if action==2 and state!=3: raise Stop('network did not pause')
    if unit==0xffffffff: return None
    if unit>99999 or stage!=16 or completed!=16 or selector!=39 or last_status or scalars[0] or scalars[1] or scalars[3]!=1:
        raise Stop('no completed netup identity for this interface')
    mac_bits=scalars[5]
    if not 0<mac_bits<0xffffffffffff or (mac_bits>>40)&1: raise Stop('invalid interface MAC')
    mac=':'.join(f'{(mac_bits>>shift)&255:02x}' for shift in range(40,-1,-8))
    return f'en{unit}',mac

def run(command):
    p=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    if p.returncode: raise Stop(f'command exit {p.returncode}: {command[0]}: {p.stderr.strip()}')
    return p.stdout

def control(c,action,execute):
    command=[c['cli'],'autonet',action,c['expectedBuild'],'json']
    if not execute:
        return {'dryRun':True,'control':command,'then':'verify returned BSD interface and MAC before OS configuration',
                'dhcp':bool(c['dhcp'] and action=='enable'),'retry':False}
    envelope=json.loads(run(command))
    identity=decode_control(envelope,{'status':0,'enable':1,'disable':2}[action])
    if action=='status' or identity is None: return envelope
    name,mac=identity
    # Resolve through the result for THIS connected service, then compare against
    # the OS. Never assume CX3 is en2 or use a cached name from a previous boot.
    text=run(['/sbin/ifconfig',name])
    if not re.search(r'\bether\s+'+re.escape(mac)+r'(?:\s|$)',text,re.I): raise Stop('OS interface MAC mismatch')
    run(['/sbin/ifconfig',name,'up' if action=='enable' else 'down'])
    if c['dhcp'] and action=='enable': run(['/usr/sbin/ipconfig','set',name,'DHCP'])
    # ipconfig starts DHCP asynchronously: this is not a lease/connectivity PASS.
    return {'control':envelope,'interface':name,'mac':mac,'dhcpRequested':bool(c['dhcp'] and action=='enable'),
            'leaseVerified':False}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--config',required=True)
    p.add_argument('--action',choices=['start','status','disable'],default='start')
    p.add_argument('--execute',action='store_true');a=p.parse_args()
    try:
        c=load_config(a.config)
        if a.action=='start' and not c['enabled']:
            print(json.dumps({'skipped':True,'reason':'configuration disabled','hardwareAccess':False}));return 0
        print(json.dumps(control(c,{'start':'enable','status':'status','disable':'disable'}[a.action],a.execute),ensure_ascii=False));return 0
    except (Stop,OSError,ValueError) as e:
        print(f'STOP: {e}; no retry/reset/cleanup',file=sys.stderr);return 1
if __name__=='__main__':sys.exit(main())
