#!/usr/bin/env python3
"""Offline retry wrapper checks; no production access or service operations."""
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import event_store_104_retry as target


class RetryTest(unittest.TestCase):
    def setUp(self):
        self.stack=contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.root=Path(self.stack.enter_context(tempfile.TemporaryDirectory())).resolve()
        self.rollback=self.root/'rollback';self.rollback.mkdir()
        for name in ('events.db','history.db'):
            (self.rollback/name).write_bytes(b'fixture retained')
        self.app=self.root/'app.json'
        self.evidence=self.root/'routes.json'
        self.value={'mqtt':{'offlineBuffer':{'eventOutbox':{
            'sqlitePath':str(self.rollback/'events.db')}}},
            'alarmStore':{'sqlitePath':str(self.rollback/'history.db')}}
        self.stack.enter_context(mock.patch.object(target,'ROLLBACK',self.rollback))
        self.stack.enter_context(mock.patch.object(target.c,'APP',self.app))
        for name in ('RELEASE','CONTROL','DATA','ARCHIVE','HISTORY','CONFIG_GENERATION','EXTRACT_EXTRA_ARGS','HISTORY_EXTRA_ARGS'):
            self.stack.enter_context(mock.patch.object(target.c,name,getattr(target.c,name)))
        self.oldmain=self.stack.enter_context(mock.patch.object(target.c,'main',return_value=7))
        self.service=self.stack.enter_context(mock.patch.object(target.c,'service',side_effect=AssertionError('no services')))
        self.argv=['retry','--execute','--route-evidence',str(self.evidence)]

    def write(self,value=None):
        self.app.write_text(json.dumps(self.value if value is None else value),encoding='utf-8')
        self.evidence.write_text(json.dumps({'appSha256':target.c.deploy.digest(self.app)}),encoding='utf-8')

    def run_main(self,argv=None):
        with mock.patch('sys.argv',self.argv if argv is None else argv):
            return target.main()

    @unittest.skipUnless(os.name=='posix','Linux canonical path contract')
    def test_fixed_namespace_and_current_sources(self):
        for store in (None,{}, {'backend':'legacy'}):
            with self.subTest(store=store):
                value=copy.deepcopy(self.value)
                if store is not None:value['eventStore']=store
                self.write(value)
                self.assertEqual(self.run_main(),7)
                self.assertEqual(target.c.RELEASE,Path('/opt/modbus-gateway/releases/event-store-104-r5-20260909'))
                self.assertEqual(target.c.CONTROL,target.c.RELEASE/'cutover')
                self.assertEqual(target.c.DATA,Path('/opt/modbus-gateway/data/event-store-104-r5-20260909'))
                self.assertEqual(target.c.CONFIG_GENERATION,'canary-r5-20260909')
                self.assertEqual(target.c.EXTRACT_EXTRA_ARGS,['--allow-rollback-schema'])
                self.assertEqual(target.c.HISTORY_EXTRA_ARGS,['--rebind-rollback-history'])
                self.assertEqual(target.c.ARCHIVE,self.rollback/'events.db')
                self.assertEqual(target.c.HISTORY,self.rollback/'history.db')
                self.assertEqual(target.c.ARCHIVE.read_bytes(),b'fixture retained')
                self.assertFalse(target.c.CONTROL.exists())
        self.service.assert_not_called()

    def test_authorization_evidence_and_unknown_arguments(self):
        for args in ([],['--execute'],['--route-evidence','unused'],
                     self.argv[1:]+['--release','/old']):
            with self.subTest(args=args),contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):self.run_main(['retry']+args)
        self.oldmain.assert_not_called()

    def test_ipc_unknown_and_malformed_backend_rejected(self):
        for store in ({'backend':'ipc-lab'},{'backend':'ipc'},{'backend':'unknown'},
                      {'backend':None},None,[]):
            with self.subTest(store=store):
                self.write(dict(self.value,eventStore=store))
                with self.assertRaisesRegex(RuntimeError,'legacy backend'):self.run_main()
        self.oldmain.assert_not_called()

    def test_wrong_archive_or_history_rejected(self):
        for field in ('archive','history'):
            for path in ('/old/archive.db',str(self.rollback/'wrong.db'),
                         str(self.rollback/'nested'/'..'/'events.db')):
                with self.subTest(field=field,path=path):
                    value=copy.deepcopy(self.value)
                    if field=='archive':value['mqtt']['offlineBuffer']['eventOutbox']['sqlitePath']=path
                    else:value['alarmStore']['sqlitePath']=path
                    self.write(value)
                    with mock.patch.object(target.c.deploy,'safe_path',side_effect=Path):
                        with self.assertRaisesRegex(RuntimeError,'exact previous rollback'):self.run_main()
        self.oldmain.assert_not_called()

    @unittest.skipUnless(os.name=='posix','Linux canonical path contract')
    def test_missing_or_symlink_database_rejected(self):
        self.write()
        events=self.rollback/'events.db';events.unlink()
        with self.assertRaisesRegex(RuntimeError,'existing regular file'):self.run_main()
        other=self.root/'other.db';other.write_bytes(b'not approved')
        events.symlink_to(other)
        with self.assertRaisesRegex(RuntimeError,'noncanonical/symlink'):self.run_main()
        self.oldmain.assert_not_called()

    def test_stale_evidence_rejected_before_configuration(self):
        self.write()
        self.app.write_text('{}',encoding='utf-8')
        before=target.c.RELEASE
        with self.assertRaisesRegex(RuntimeError,'configuration changed'):self.run_main()
        self.assertEqual(target.c.RELEASE,before)
        self.oldmain.assert_not_called()

    def test_duplicate_keys_rejected(self):
        self.app.write_text('{"eventStore":{},"eventStore":{}}',encoding='utf-8')
        self.evidence.write_text(json.dumps({'appSha256':target.c.deploy.digest(self.app)}),encoding='utf-8')
        with self.assertRaisesRegex(RuntimeError,'duplicate JSON key'):self.run_main()
        self.oldmain.assert_not_called()

    def test_default_cutover_generation_unchanged(self):
        self.assertEqual(target.c.CONFIG_GENERATION,'canary-r05-20260908')
        self.assertEqual(target.c.EXTRACT_EXTRA_ARGS,[])
        self.assertEqual(target.c.HISTORY_EXTRA_ARGS,[])


if __name__=='__main__':unittest.main()
