"""Check restore guards and mocked Windows API semantics; never rebind a device."""
from pathlib import Path
import ctypes
import importlib.util
import json
import sys
import unittest
from unittest.mock import Mock,patch

path=Path(__file__).with_name('restore-stock-mt7922.py')
spec=importlib.util.spec_from_file_location('recovery',path)
recovery=importlib.util.module_from_spec(spec);spec.loader.exec_module(recovery)

class Checks(unittest.TestCase):
    def test_exact_instance(self):
        self.assertEqual(recovery.validate_instances([recovery.INSTANCE_ID.lower()]),[recovery.INSTANCE_ID.lower()])
        for instances in ([],[recovery.INSTANCE_ID,recovery.INSTANCE_ID],[recovery.HARDWARE_ID+'\\different'],['PCI\\VEN_10EC&DEV_8168\\ethernet']):
            with self.subTest(instances=instances),self.assertRaises(ValueError):recovery.validate_instances(instances)

    def test_validation_precedes_change(self):
        for stage in ('checked_backup','exact_present_devices'):
            with patch.object(sys,'argv',['recovery','--restore']), \
                 patch.object(recovery,'checked_backup',return_value=Path('stock.inf')) as backup, \
                 patch.object(recovery,'exact_present_devices',return_value=[recovery.INSTANCE_ID]) as devices, \
                 patch.object(recovery,'restore') as restore:
                (backup if stage=='checked_backup' else devices).side_effect=ValueError('rejected')
                with self.assertRaises(ValueError):recovery.main()
                restore.assert_not_called()

    def test_default_never_changes_driver(self):
        with patch.object(sys,'argv',['recovery']), \
             patch.object(recovery,'checked_backup',return_value=Path('stock.inf')), \
             patch.object(recovery,'exact_present_devices',return_value=[recovery.INSTANCE_ID]), \
             patch.object(recovery,'restore') as restore,patch('builtins.print') as output:
            recovery.main();restore.assert_not_called()
            self.assertFalse(json.loads(output.call_args.args[0])['restoration_executed'])

    def test_elevation_before_loading_installer(self):
        with patch.object(recovery.c.windll.shell32,'IsUserAnAdmin',return_value=False),patch.object(recovery.c,'WinDLL') as loader:
            with self.assertRaises(PermissionError):recovery.restore(Path('stock.inf'))
            loader.assert_not_called()

    def test_only_exact_id_and_explicit_reboot_result(self):
        def install(hwnd,hardware,inf,flags,reboot):
            self.assertIsNone(hwnd);self.assertEqual(hardware,recovery.HARDWARE_ID)
            self.assertEqual(inf,str(Path('stock.inf')));self.assertEqual(flags,1)
            self.assertIsNotNone(reboot)
            ctypes.cast(reboot,ctypes.POINTER(recovery.w.BOOL)).contents.value=True
            return True
        api=Mock();api.UpdateDriverForPlugAndPlayDevicesW=Mock(side_effect=install)
        with patch.object(recovery.c.windll.shell32,'IsUserAnAdmin',return_value=True),patch.object(recovery.c,'WinDLL',return_value=api):
            self.assertTrue(recovery.restore(Path('stock.inf')))
            api.UpdateDriverForPlugAndPlayDevicesW.assert_called_once()

    def test_installer_failure_is_not_success(self):
        api=Mock();api.UpdateDriverForPlugAndPlayDevicesW=Mock(return_value=False)
        with patch.object(recovery.c.windll.shell32,'IsUserAnAdmin',return_value=True),patch.object(recovery.c,'WinDLL',return_value=api), \
             patch.object(recovery.c,'get_last_error',return_value=5):
            with self.assertRaises(OSError):recovery.restore(Path('stock.inf'))

if __name__=='__main__':unittest.main()
