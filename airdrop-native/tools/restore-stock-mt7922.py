"""Check the pinned stock-driver recovery copy; restore only with explicit --restore.

Does not sign, change boot policy, disable adapters, delete driver packages or
reboot. --restore requires elevation and rebinds only the unique exact MT7922.
"""
from pathlib import Path
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import subprocess

ROOT=Path(__file__).resolve().parents[1]
BACKUP=ROOT/'artifacts/stock-driver-restore-01'
RECEIPT_SHA='3e617f7a5684a4083b2540c93e3db9a20c207ffff922d00d1394ff18fa07ce63'
HARDWARE_ID=r'PCI\VEN_14C3&DEV_0616&SUBSYS_223C14C3&REV_02'
INSTANCE_ID=HARDWARE_ID+r'\4&FB64F38&0&000F'
SIGNTOOL=Path(r'C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x64\signtool.exe')

def checked_backup(folder):
    folder=folder.resolve(strict=True)
    contents=(folder/'backup-receipt.json').read_bytes()
    if hashlib.sha256(contents).hexdigest()!=RECEIPT_SHA:raise ValueError('recovery receipt differs from pinned copy')
    receipt=json.loads(contents)
    for name,expected in receipt['files'].items():
        raw=folder/name
        if raw.is_symlink():raise ValueError('recovery files must be regular copies')
        p=raw.resolve(strict=True)
        if p.parent!=folder:raise ValueError('recovery file escaped the backup directory')
        if hashlib.sha256(p.read_bytes()).hexdigest()!=expected['sha256']:raise ValueError('recovery file hash differs: '+name)
    if receipt['hardware_id']!=HARDWARE_ID:raise ValueError('recovery hardware scope differs')
    # Recheck catalog membership under kernel signing policy at execution time.
    for name in receipt['files']:
        if name.lower().endswith('.cat'):continue
        result=subprocess.run([str(SIGNTOOL),'verify','/kp','/c',str(folder/'mtkwl6ex.cat'),str(folder/name)],
                              capture_output=True,text=True,timeout=30)
        if result.returncode:raise ValueError('kernel-policy catalog verification failed: '+name)
    return folder/'mtkwl6ex.inf'

class DeviceInfo(c.Structure):
    _fields_=[('size',w.DWORD),('guid',c.c_ubyte*16),('instance',w.DWORD),('reserved',c.c_size_t)]

def validate_instances(matches):
    if len(matches)!=1 or matches[0].upper()!=INSTANCE_ID.upper():
        raise ValueError('expected exactly the recorded MT7922 instance; no driver change permitted')
    return matches

def exact_present_devices():
    api=c.WinDLL('setupapi',use_last_error=True)
    api.SetupDiGetClassDevsW.argtypes=[c.c_void_p,w.LPCWSTR,w.HWND,w.DWORD]
    api.SetupDiGetClassDevsW.restype=w.HANDLE
    api.SetupDiEnumDeviceInfo.argtypes=[w.HANDLE,w.DWORD,c.POINTER(DeviceInfo)]
    api.SetupDiEnumDeviceInfo.restype=w.BOOL
    api.SetupDiGetDeviceRegistryPropertyW.argtypes=[w.HANDLE,c.POINTER(DeviceInfo),w.DWORD,c.c_void_p,c.c_void_p,w.DWORD,c.POINTER(w.DWORD)]
    api.SetupDiGetDeviceRegistryPropertyW.restype=w.BOOL
    api.SetupDiGetDeviceInstanceIdW.argtypes=[w.HANDLE,c.POINTER(DeviceInfo),w.LPWSTR,w.DWORD,c.c_void_p]
    api.SetupDiGetDeviceInstanceIdW.restype=w.BOOL
    api.SetupDiDestroyDeviceInfoList.argtypes=[w.HANDLE]
    api.SetupDiDestroyDeviceInfoList.restype=w.BOOL
    handle=api.SetupDiGetClassDevsW(None,'PCI',None,2|4)
    if handle==c.c_void_p(-1).value:raise c.WinError(c.get_last_error())
    matches=[]
    try:
        for index in range(4096):
            data=DeviceInfo();data.size=c.sizeof(data)
            if not api.SetupDiEnumDeviceInfo(handle,index,c.byref(data)):
                if c.get_last_error()==259:break
                raise c.WinError(c.get_last_error())
            buffer=c.create_string_buffer(65536);required=w.DWORD()
            if not api.SetupDiGetDeviceRegistryPropertyW(handle,c.byref(data),1,None,buffer,len(buffer),c.byref(required)):
                if c.get_last_error()==13:continue
                raise c.WinError(c.get_last_error())
            if required.value>len(buffer) or required.value%2:raise ValueError('invalid device hardware-ID buffer')
            ids=buffer.raw[:required.value].decode('utf-16-le').rstrip('\0').split('\0')
            if HARDWARE_ID.upper() not in [item.upper() for item in ids]:continue
            instance=c.create_unicode_buffer(1024)
            if not api.SetupDiGetDeviceInstanceIdW(handle,c.byref(data),instance,len(instance),None):raise c.WinError(c.get_last_error())
            matches.append(instance.value)
        else:raise RuntimeError('device enumeration exceeded bound')
    finally:api.SetupDiDestroyDeviceInfoList(handle)
    return validate_instances(matches)

def restore(inf):
    if not c.windll.shell32.IsUserAnAdmin():raise PermissionError('--restore requires an elevated Windows process')
    api=c.WinDLL('newdev',use_last_error=True)
    fn=api.UpdateDriverForPlugAndPlayDevicesW
    fn.argtypes=[w.HWND,w.LPCWSTR,w.LPCWSTR,w.DWORD,c.POINTER(w.BOOL)];fn.restype=w.BOOL
    reboot=w.BOOL(False)
    # FORCE permits the recorded older stock package to replace the candidate.
    # A non-null reboot pointer prevents the API's automatic restart prompt.
    if not fn(None,HARDWARE_ID,str(inf),1,c.byref(reboot)):raise c.WinError(c.get_last_error())
    return bool(reboot.value)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--restore',action='store_true')
    p.add_argument('--backup',type=Path,default=BACKUP)
    args=p.parse_args()
    if c.sizeof(c.c_void_p)!=8:raise RuntimeError('use native x64 Python')
    inf=checked_backup(args.backup);devices=exact_present_devices()
    report={'recovery_files_valid':True,'kernel_catalog_verified':True,'exact_devices':devices,'restore_requested':args.restore,
            'restoration_executed':False,'reboot_performed':False,'boot_policy_changed':False}
    if args.restore:
        report['reboot_required']=restore(inf);report['restoration_executed']=True
    print(json.dumps(report,indent=2))

if __name__=='__main__':main()
