#!/usr/bin/env python3
"""Validate human-readable report structure and counting semantics; synthetic cases are labelled."""
import argparse
from html.parser import HTMLParser
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
import event_store_soak_report as report


class Tables(HTMLParser):
    def __init__(self):
        super().__init__()
        self.tags = []
        self.rows = []
        self.current = None
        self.headers = []
        self.in_header = False

    def handle_starttag(self, tag, attrs):
        self.tags.append(tag)
        if tag == 'tr':
            self.current = 0
        if tag in ('td', 'th') and self.current is not None:
            self.current += 1
        if tag == 'th':
            self.in_header = True

    def handle_data(self, data):
        if self.in_header:
            self.headers.append(data)

    def handle_endtag(self, tag):
        if tag == 'tr':
            self.rows.append(self.current)
            self.current = None
        if tag == 'th':
            self.in_header = False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='existing real report directory')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    cases = []

    def check(name, action):
        try:
            action()
            cases.append(dict(name=name, passed=True))
        except Exception as error:
            cases.append(dict(name=name, passed=False, error=repr(error)))

    def tables():
        html = (args.source / 'report.html').read_text(encoding='utf-8')
        md = (args.source / 'report.md').read_text(encoding='utf-8')
        parsed = Tables()
        parsed.feed(html)
        assert len(parsed.rows) > 50 and all(row == 4 for row in parsed.rows)
        assert parsed.tags.count('table') >= 8 and 'pre' not in parsed.tags and 'script' not in parsed.tags
        assert parsed.headers[:4] == ['指标', '实测值', '判定', '单位']
        assert '```' not in md and '```' not in html
        for text in ('关键结论', '未覆盖项', '已发起提交 submitted', '已确认提交 committed', '未知响应', '重复 Claim'):
            assert text in html and text in md
        raw = json.loads((args.source / 'report.json').read_text())
        assert raw['counts']['submitted'] == raw['counts']['committed']
        assert report.verdict_label(raw) in html and not raw['accepted24h']
    check('真实短测中文 Markdown 与 HTML 四列表格，JSON 独立可解析', tables)

    def counters():
        path = args.output / 'synthetic-operations.jsonl'
        records = []
        def request(ids, op='AppendEventsAndStates'):
            records.append(dict(kind='request', op=op, args=dict(events=[dict(eventId=x) for x in ids])))
        def attempt(number, outcome):
            records.extend([dict(kind='rpc_attempt', attempt=number), dict(kind=outcome, attempt=number)])
        request(['a', 'b'])
        attempt(0, 'unknown')
        attempt(1, 'unknown')
        attempt(2, 'response')
        request(['b', 'c'])
        attempt(0, 'response')
        request([] , 'AckBatch')
        attempt(0, 'unknown')
        attempt(1, 'rejected')
        request(['d'])
        attempt(0, 'unknown')
        path.write_text(''.join(json.dumps(row) + '\n' for row in records), encoding='utf-8')
        result = report.operation_counts(path, True)
        expected = dict(submittedBatches=3, submittedEvents=4, submittedEventOccurrences=5,
            duplicateSubmittedEventOccurrences=1, unknownResponses=4, unknownRequests=3,
            recoveredUnknownRequests=1, rejectedUnknownRequests=1, unresolvedUnknownRequests=1,
            rpcAttempts=7, retryAttempts=3, rejectedRequests=1, incompleteLogicalRequests=1)
        for key, value in expected.items():
            assert result[key] == value, (key, result)
        legacy = args.output / 'synthetic-legacy.jsonl'
        legacy.write_text(''.join(json.dumps(row) + '\n' for row in records if row['kind'] != 'rpc_attempt'), encoding='utf-8')
        assert report.operation_counts(legacy)['retryAttempts'] is None
        assert report.operation_counts(args.output / 'absent.jsonl')['submittedEvents'] is None
        path.write_text(path.read_text() + '{incomplete\n', encoding='utf-8')
        assert report.operation_counts(path)['malformedLines'] == 1
    check('事件去重、批数、未知次数/请求、恢复/拒绝/未决与重复 RPC 分离', counters)

    def escaped():
        data = json.loads((args.source / 'report.json').read_text())
        data.update(error='</td><script>oops</script>|新行\n下一行', syntheticReportFixture=True)
        report.render(args.output, data)
        html = (args.output / 'report.html').read_text(encoding='utf-8')
        parsed = Tables()
        parsed.feed(html)
        assert 'script' not in parsed.tags and 'pre' not in parsed.tags
        assert '&lt;script&gt;' in html and '合成报告夹具' in html
        assert all(row == 4 for row in parsed.rows)
    check('诊断转义与合成报告标识', escaped)
    result = dict(syntheticCounterFixtures=True, accepted24h=False, passed=all(row['passed'] for row in cases), cases=cases)
    (args.output / 'selftest.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
