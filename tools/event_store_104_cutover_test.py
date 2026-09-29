#!/usr/bin/env python3
import contextlib
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import event_store_104_cutover as target


class RecoveryTest(unittest.TestCase):
    def setUp(self):
        self.stack=contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.temp=self.stack.enter_context(tempfile.TemporaryDirectory())
        self.root=Path(self.temp)
        self.app=self.root/'app.json';self.app.write_text('original')
        self.original=self.root/'original.json';self.original.write_text('original')
        self.store=self.root/'store.service'
        self.start=self.root/'ready';self.start.write_text('ready')
        self.calls=[]
        for name,value in [('APP',self.app),('STORE_FILE',self.store),('START_GATE',self.start),('CONTROL',self.root)]:
            self.stack.enter_context(mock.patch.object(target,name,value))
        self.stop=self.stack.enter_context(mock.patch.object(target,'stop',side_effect=lambda units:self.calls.append(('stop',list(units)))))
        self.invoke=self.stack.enter_context(mock.patch.object(target,'invoke',side_effect=lambda *args:self.calls.append(('rollback',args))))
        self.stack.enter_context(mock.patch.object(target,'gate',return_value=self.root/'gate.json'))
        self.stack.enter_context(mock.patch.object(target,'remove_guards',side_effect=lambda:self.calls.append(('remove-guards',))))
        def service(*args):
            self.calls.append(('service',args))
            return 'active\n'
        self.service=self.stack.enter_context(mock.patch.object(target,'service',side_effect=service))

    def starts(self):
        return [c for c in self.calls if c[0]=='service' and 'start' in c[1]]

    def test_pre_activation_failure_restores_original_without_rollback(self):
        target.recover(self.original,{},False)
        self.assertEqual(len(self.starts()),3)
        self.invoke.assert_not_called()
        self.assertFalse(self.start.exists())

    def test_active_failure_rolls_back_before_restarting(self):
        self.app.write_text('new-path')
        self.store.write_text('new-service')
        target.recover(self.original,{},True)
        self.assertEqual(len(self.starts()),3)
        recovery=next(i for i,c in enumerate(self.calls) if c[0]=='rollback')
        restart=next(i for i,c in enumerate(self.calls) if c[0]=='service' and 'start' in c[1])
        self.assertLess(recovery,restart)
        self.assertEqual(self.calls[1],('stop',[target.deploy.STORE_UNIT]))

    def test_rollback_failure_keeps_writers_stopped(self):
        self.app.write_text('new-path')
        self.store.write_text('new-service')
        self.invoke.side_effect=RuntimeError('merge failed')
        with self.assertRaisesRegex(RuntimeError,'merge failed'):
            target.recover(self.original,{},False)
        self.assertFalse(self.starts())
        self.assertFalse(any(c[0]=='remove-guards' for c in self.calls))

    def test_stop_failure_never_attempts_database_recovery(self):
        self.stop.side_effect=RuntimeError('writer did not stop')
        with self.assertRaisesRegex(RuntimeError,'writer did not stop'):
            target.recover(self.original,{},False)
        self.invoke.assert_not_called()
        self.assertFalse(self.starts())

    def test_restore_failure_is_not_reported_as_running(self):
        self.service.side_effect=lambda *args:'failed\n' if args[0]=='show' else ''
        with self.assertRaisesRegex(RuntimeError,'restored service failed'):
            target.recover(self.original,{},True)

    def test_successful_rollback_does_not_rewrite_original_archive(self):
        self.app.write_text('new-path');self.store.write_text('new-service')
        archive=self.root/'archive.db';archive.write_bytes(b'old-full-history')
        target.recover(self.original,{str(archive):0o644},True)
        self.assertEqual(archive.read_bytes(),b'old-full-history')
        self.assertEqual(self.original.read_text(),'original')


if __name__=='__main__':unittest.main()
