import sqlite3
from contextlib import closing
import tempfile
from pathlib import Path
import unittest
from event_history_query import query

class HistoryQueryTest(unittest.TestCase):
    def test_lagging_projection_and_legacy_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            source, history = Path(directory) / 'source.db', Path(directory) / 'history.db'
            columns = ('event_id TEXT,point_index INTEGER,ts INTEGER,alarm_type TEXT,active INTEGER,'
                       'threshold REAL,value REAL,quality INTEGER,stale INTEGER,persist_value TEXT,'
                       'gateway_code TEXT,device_code TEXT,point_code TEXT')
            event = ('new', 1, 100, 'high', 1, 50, 60, 1, 0, 'fault', 'machine', 'meter', 'point')
            with closing(sqlite3.connect(source, isolation_level=None)) as db:
                db.executescript("CREATE TABLE event_store_identity(id,store_id); INSERT INTO event_store_identity VALUES(1,'s');"
                                 "CREATE TABLE event_journal_meta(id,journal_generation); INSERT INTO event_journal_meta VALUES(1,'g');"
                                 'CREATE TABLE event_local_journal(kind TEXT,' + columns + ');')
                db.execute('INSERT INTO event_local_journal VALUES(' + ','.join('?' * 14) + ')', ('alarm',) + event)
            with closing(sqlite3.connect(history, isolation_level=None)) as db:
                db.executescript("CREATE TABLE alarm_projection_meta(id,store_id,journal_generation);"
                                 "INSERT INTO alarm_projection_meta VALUES(1,'s','g');"
                                 'CREATE TABLE alarm_events(' + columns + ');')
                db.execute('INSERT INTO alarm_events VALUES(' + ','.join('?' * 13) + ')', ('old',) + event[1:])
            result = query(source, history, 0, 200)
            self.assertEqual({r['event_id'] for r in result['alarms']}, {'old', 'new'})
            self.assertEqual(query(source, history, 101, 200)['count'], 0)
            self.assertEqual(query(source, history, 0, 200, machine='missing')['count'], 0)
            self.assertEqual(query(source, history, 0, 200, meter='missing')['count'], 0)
            self.assertEqual(query(source, history, 100, 100, machine='machine', meter='meter')['count'], 2)
            for begin, end, limit in [(-1, 200, 1), (200, 100, 1), (0, 200, 0), (0, 200, 10001)]:
                with self.assertRaises(ValueError): query(source, history, begin, end, limit=limit)
            self.assertTrue(query(source, history, 0, 200, limit=1)['truncated'])
            with closing(sqlite3.connect(history, isolation_level=None)) as db:
                for empty in ('', None):
                    db.execute('INSERT INTO alarm_events VALUES(' + ','.join('?' * 13) + ')', (empty,) + event[1:])
            self.assertEqual(query(source, history, 0, 200)['count'], 4)
            with closing(sqlite3.connect(source, isolation_level=None)) as db:
                db.execute('INSERT INTO event_local_journal VALUES(' + ','.join('?' * 14) + ')', ('alarm', '') + event[1:])
            with self.assertRaisesRegex(ValueError, 'invalid event identity'):
                query(source, history, 0, 200)
            with closing(sqlite3.connect(source, isolation_level=None)) as db:
                db.execute("DELETE FROM event_local_journal WHERE event_id=''")
            with closing(sqlite3.connect(history, isolation_level=None)) as db:
                db.execute("DELETE FROM alarm_events WHERE event_id='' OR event_id IS NULL")
            with closing(sqlite3.connect(history, isolation_level=None)) as db:
                db.execute('INSERT INTO alarm_events VALUES(' + ','.join('?' * 13) + ')', event)
            self.assertEqual(query(source, history, 0, 200)['count'], 2)
            with closing(sqlite3.connect(history, isolation_level=None)) as db:
                db.execute("UPDATE alarm_events SET value=999 WHERE event_id='new'")
            with self.assertRaises(ValueError): query(source, history, 0, 200)
            with closing(sqlite3.connect(source, isolation_level=None)) as db:
                for offset in range(3):
                    early = ('early-' + str(offset), 1, offset + 1) + event[3:]
                    db.execute('INSERT INTO event_local_journal VALUES(' + ','.join('?' * 14) + ')', ('alarm',) + early)
            for limit in (1, 2, 100):
                with self.assertRaisesRegex(ValueError, 'conflicts'):
                    query(source, history, 0, 200, limit=limit)
            self.assertEqual(query(source, history, 0, 50, limit=1)['count'], 1)
            self.assertEqual(query(source, history, 0, 200, meter='missing')['count'], 0)
            with closing(sqlite3.connect(history, isolation_level=None)) as db:
                db.execute("UPDATE alarm_events SET value=60 WHERE event_id='new'")
                db.execute('INSERT INTO alarm_events VALUES(' + ','.join('?' * 13) + ')', event)
            with self.assertRaisesRegex(ValueError, 'conflicts'):
                query(source, history, 0, 200, limit=1)
            with closing(sqlite3.connect(history, isolation_level=None)) as db:
                db.execute("UPDATE alarm_projection_meta SET journal_generation='other'")
            with self.assertRaises(ValueError): query(source, history, 0, 200)
            with self.assertRaises(ValueError): query(source, source, 0, 200)

if __name__ == '__main__': unittest.main()
