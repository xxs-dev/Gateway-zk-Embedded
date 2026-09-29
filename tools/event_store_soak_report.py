"""中文人工报告与有明确单位的流水计数；机器数据仍由 report.json 保存。"""
import html
import json
import sqlite3


VERDICTS = {
    'PREFLIGHT_PASSED': '能力预检通过，不代表长跑验收',
    'PASS_24H_LAB': '24 小时实验链路验收通过，不代表生产验收',
    'PASS_TOOL_SELFTEST': '短测工具自测通过，未完成 24 小时验收',
    'FAILED_RATE_TARGET': '目标生成速率未达标',
    'COMPLETE_EVENTS_ONLY': '仅完成旧 events 链路，不覆盖新 full 目标',
    'COMPLETE_SHORT_LAB_ONLY': '仅完成短时实验，未满足 24 小时门槛',
    'INCOMPLETE_OR_FAILED': '运行未完成或校验失败',
}


def verdict_label(result):
    if result.get('syntheticReportFixture'):
        return '合成报告夹具，不是实际运行或 24 小时证据'
    return VERDICTS.get(result.get('verdict'), '结果未知，需核查证据')


def operation_counts(path, attempt_logging=False):
    """Stream one active logical RPC at a time; unique event IDs use a temporary disk DB."""
    names = ('logicalRequests', 'submittedBatches', 'submittedEventOccurrences', 'submittedEvents',
             'duplicateSubmittedEventOccurrences', 'rpcAttempts', 'retryAttempts', 'unknownResponses',
             'unknownRequests', 'recoveredUnknownRequests', 'unresolvedUnknownRequests',
             'rejectedRequests', 'rejectedUnknownRequests', 'injectedReplyLosses')
    result = dict.fromkeys(names, 0)
    result.update(available=path.is_file(), malformedLines=0, incompleteLogicalRequests=0,
                  attemptLogging=attempt_logging)
    if not path.is_file():
        result.update(dict.fromkeys(names))
        result['incompleteLogicalRequests'] = None
        return result
    # Empty filename creates an automatically removed disk-backed temporary DB.
    seen = sqlite3.connect('')
    seen.execute('PRAGMA cache_size=-2048')
    seen.execute('CREATE TABLE submitted(eventId TEXT PRIMARY KEY)')
    current = None

    def unfinished():
        if current is not None:
            result['incompleteLogicalRequests'] += 1
            result['unresolvedUnknownRequests'] += int(current['unknown'])

    try:
        with path.open(encoding='utf-8') as source:
            for line in source:
                try:
                    row = json.loads(line)
                    kind = row['kind']
                    if kind == 'request':
                        unfinished()
                        current = dict(unknown=False, op=row['op'])
                        result['logicalRequests'] += 1
                        if row['op'] == 'AppendEventsAndStates':
                            result['submittedBatches'] += 1
                            for event in row['args']['events']:
                                result['submittedEventOccurrences'] += 1
                                inserted = seen.execute('INSERT OR IGNORE INTO submitted VALUES(?)', (event['eventId'],)).rowcount
                                result['submittedEvents'] += inserted
                                result['duplicateSubmittedEventOccurrences'] += 1 - inserted
                    elif kind == 'rpc_attempt':
                        result['attemptLogging'] = True
                        result['rpcAttempts'] += 1
                        result['retryAttempts'] += int(row['attempt'] > 0)
                    elif kind == 'unknown':
                        result['unknownResponses'] += 1
                        if current and not current['unknown']:
                            result['unknownRequests'] += 1
                            current['unknown'] = True
                    elif kind == 'response':
                        if current and current['unknown']:
                            result['recoveredUnknownRequests'] += 1
                        current = None
                    elif kind == 'rejected':
                        result['rejectedRequests'] += 1
                        result['rejectedUnknownRequests'] += int(bool(current and current['unknown']))
                        current = None
                    elif kind == 'injected_reply_loss':
                        result['injectedReplyLosses'] += 1
                except (ValueError, KeyError, TypeError):
                    result['malformedLines'] += 1
        unfinished()
    finally:
        seen.close()
    if not result['attemptLogging']:
        result['rpcAttempts'] = result['retryAttempts'] = None
    return result


def value_text(value):
    if value is None:
        return '未采集 / 不可用'
    if isinstance(value, bool):
        return '是' if value else '否'
    if isinstance(value, float):
        return f'{value:,.3f}'
    if isinstance(value, int):
        return f'{value:,}'
    return str(value)


def sections(result):
    counts, config = result['counts'], result['config']
    ops, projection = result['operations'], result['journalProjection']
    full = result.get('coverageMode') == 'full'
    tables = []

    def add(title, rows):
        tables.append((title, rows))

    def zero(number):
        return '未采集' if number is None else '通过' if number == 0 else '需核查'

    add('运行范围与验收门槛', [
        ('测试模式', 'full：状态 CAS、投递、源 journal 与历史投影' if full else 'events：仅旧投递链路', '实测范围', '—'),
        ('计划负载时长', config.get('duration'), '配置值', '秒'),
        ('实际负载时长', result.get('actualLoadSeconds'),
         '达到时长门槛' if result.get('actualLoadSeconds', 0) >= 86400 else '未达到 86400 秒', '秒'),
        ('投递排空时长', result.get('drainSeconds'), '不计入 24 小时负载', '秒'),
        ('投影排空时长', result.get('projectionDrainSeconds'), '不计入 24 小时负载', '秒'),
        ('单次 RPC 超时', config.get('rpc_timeout'), 'soak 独立配置；每个逻辑请求最多4次尝试', '秒'),
        ('计划重启停机时间', result.get('downtimeSeconds'), '计入负载墙钟，非持续在线时长', '秒'),
        ('24 小时 full 验收', result.get('accepted24h'), '通过' if result.get('accepted24h') else '未通过 / 未执行', '—'),
        ('实际数据库文件系统', result.get('databaseFilesystem'), 'tmpfs 为内存文件系统，不证明磁盘耐久性', '—'),
    ])
    add('事件与批次对账', [
        ('计划生成事件', result.get('targetEvents'), '配置目标', '事件'),
        ('已生成事件', counts['generated'], '按 eventId 去重', '事件'),
        ('已发起提交 submitted', ops['submittedEvents'], '按 eventId 去重，不代表服务端已接收', '事件'),
        ('提交中携带的事件条目', ops['submittedEventOccurrences'], '不含 RPC 重试；可能含重复 eventId', '事件条目'),
        ('已发起提交批次', ops['submittedBatches'], '每个逻辑 Append 请求计一批，重试不加批', '批'),
        ('已确认提交 committed', counts['committed'], '按 eventId 去重，有提交回执', '事件'),
        ('已确认提交批次', result['latency']['commitConfirm']['samples'], '通过提交回执校验的批数', '批'),
        ('已 Claim 事件', counts['claimed'], '按 eventId 去重', '事件'),
        ('已确认 Ack 事件', counts['acked'], '按 eventId 去重，非 broker PUBACK', '事件'),
        ('已确认 Ack 批次', result['latency']['ackConfirm']['samples'], '完整批次通过 APPLIED 校验', '批'),
        ('已生成但未确认提交', counts['missingCommit'], zero(counts['missingCommit']), '事件'),
        ('已生成但未确认 Ack', counts['missingAck'], zero(counts['missingAck']), '事件'),
        ('最终待投递', result.get('pendingAtEnd'), zero(result.get('pendingAtEnd')), '事件'),
        ('对账异常记录', counts['anomalies'], zero(counts['anomalies']), '条记录'),
    ])
    add('未知结果、重复与拒绝', [
        ('逻辑 RPC 请求', ops['logicalRequests'], '含注册、Append、Claim、Ack；不含旁路探针', '请求'),
        ('RPC 调用尝试', ops['rpcAttempts'], '含首次与重试；旧流水未采集则不估算', '次'),
        ('重复 RPC 尝试（原请求重放）', ops['retryAttempts'], '允许的恢复行为，不等于重复写入', '次'),
        ('未知响应', ops['unknownResponses'], '每次异常响应计一次', '次'),
        ('出现未知结果的逻辑请求', ops['unknownRequests'], '同一请求多次未知只计一次', '请求'),
        ('重试后收到有效回复的未知请求', ops['recoveredUnknownRequests'], '操作最终语义仍须通过上表对账', '请求'),
        ('最终明确拒绝的未知请求', ops['rejectedUnknownRequests'], '已消除未知，但该请求未成功', '请求'),
        ('结束时仍未决的未知请求', ops['unresolvedUnknownRequests'], zero(ops['unresolvedUnknownRequests']), '请求'),
        ('无终态的逻辑请求', ops['incompleteLogicalRequests'], zero(ops['incompleteLogicalRequests']), '请求'),
        ('明确拒绝的请求', ops['rejectedRequests'], zero(ops['rejectedRequests']), '请求'),
        ('主动丢弃回复注入', ops['injectedReplyLosses'], '工具故障注入，与未知响应可能重叠，不相加', '次'),
        ('逻辑提交中的重复 eventId 条目', ops['duplicateSubmittedEventOccurrences'],
         zero(ops['duplicateSubmittedEventOccurrences']), '额外事件条目'),
        ('重复 Claim 交付', counts['duplicateClaims'], '重复观测次数，不等于重复提交', '额外事件交付次数'),
        ('源 journal 重复写入', projection.get('duplicateJournalRows') if full else None,
         zero(projection.get('duplicateJournalRows')) if full else 'events 模式未检查', '额外行'),
        ('历史表重复写入', projection.get('duplicateHistoryRows') if full else None,
         zero(projection.get('duplicateHistoryRows')) if full else 'events 模式未检查', '额外行'),
        ('流水损坏行', ops.get('malformedLines'), zero(ops.get('malformedLines')), '行'),
    ])
    add('吞吐与速率缺口', [
        ('目标生成速率', config.get('rate'), '全部 producer 合计', '事件/秒'),
        ('实际生成吞吐', result['throughput']['generatedPerSecond'], '负载加投递/投影排空为分母', '事件/秒'),
        ('提交确认吞吐', result['throughput']['committedPerSecond'], '按事件，不是批次', '事件/秒'),
        ('Ack 确认吞吐', result['throughput']['ackedPerSecond'], '按事件，不是批次', '事件/秒'),
        ('目标生成数量缺口', counts['rateGapEvents'], '通过配置比例门槛' if result['rateThresholdPassed'] else '未达标', '事件'),
        ('最低目标完成比例', 100 * config['min_rate_ratio'], '配置门槛，不是实测比例', '%'),
    ])
    rows = []
    for kind, label, unit in (('commitConfirm', '提交确认', '批'), ('ackConfirm', 'Ack 确认', '批'),
                              ('endToEnd', '生成至 Ack', '事件')):
        metric = result['latency'][kind]
        rows.append((label + '样本数', metric['samples'], '成功样本；未知和失败单独计数', unit))
        for key, name in (('p50', 'P50'), ('p95', 'P95'), ('p99', 'P99'), ('max', '最大值')):
            rows.append((label + '延迟 ' + name, metric[key], '仅观测，未配置性能阈值', '毫秒'))
    add('延迟分位', rows)
    resource = result['resources']
    rows = [('资源采样次数', resource['samples'], '实际采样', '次'),
            ('RSS 采样最大值', resource['rssMaxKiB'], '仅观测，不是全程真实峰值', 'KiB'),
            ('PSS 采样最大值', resource['pssMaxKiB'], '仅观测，不是全程真实峰值', 'KiB'),
            ('CPU 采样最大值', resource['cpuMaxOneCorePercent'], '单核为 100%，未配置阈值', '%'),
            ('资源采样损坏行', resource['collectionErrors'], zero(resource['collectionErrors']), '行')]
    files = resource.get('sampledMaxFileBytes', {})
    for name, label in (('events.db', '源数据库'), ('events.db-wal', '源 WAL'),
                        ('history.db', '历史数据库'), ('history.db-wal', '历史 WAL')):
        rows.append((label + '采样最大大小', files.get(name), '未出现的文件记未采集，不填零', '字节'))
    add('资源与文件大小', rows)
    restarts = result.get('restarts', [])
    add('进程重启', [
        ('已完成正常重启', sum(row['mode'] == 'term' for row in restarts), '完整 RPC 边界内 SIGTERM', '次'),
        ('已完成强制重启', sum(row['mode'] == 'kill' for row in restarts), '完整 RPC 边界内 SIGKILL，非掉电', '次'),
        ('回执恢复通过', sum(bool(row.get('receiptRecovery')) for row in restarts), '仅已记录成功的重启', '次'),
        ('状态 CAS 重启对账通过', sum(bool((row.get('stateRecovery') or {}).get('passed')) for row in restarts), '核对 owner/version/value/lifecycle/sourceTs 等持久化字段', '次'),
    ])
    if full:
        state = result.get('stateReconciliation', {})
        rows = [('状态 CAS 对账', state.get('passed'), '通过' if state.get('passed') else '未通过 / 旧报告未采集', '判定'),
                ('状态键数量', state.get('expectedStates'), '每个实际提交的 producer 一个键', '键'),
                ('已确认状态更新', state.get('confirmedUpdates'), '一次 Append 批次更新一次，与事件条数不同', '批')]
        for key, label in (('pendingUpdates', '未确认状态更新'), ('missingStates', '缺少状态'), ('extraStates', '额外状态'),
                           ('mismatchStates', '状态字段不符'), ('sequenceErrors', 'CAS 版本序列异常')):
            rows.append((label, state.get(key), zero(state.get(key)), '批' if key in ('pendingUpdates', 'sequenceErrors') else '键'))
        add('基线状态 CAS 与同批提交', rows)
    if full:
        rows = [('full 逐 ID 对账', projection.get('passed'), '通过' if projection.get('passed') else '未通过', '—')]
        for key, label, unit in (
            ('expectedLocal', '生成 localEvents', '事件'), ('sourceRows', '源 journal 行数', '行'),
            ('expectedAlarms', '应投影 alarm', '事件'), ('historyRows', '历史 alarm 行数', '行'),
            ('missingJournal', '源 journal 缺行', '行'), ('journalMismatch', '源 journal 字段不符', '事件'),
            ('extraJournal', '源 journal 额外事件', '行'), ('missingHistory', '历史 alarm 缺行', '行'),
            ('historyMismatch', '历史 alarm 字段不符', '事件'), ('extraHistory', '历史额外事件', '行'),
            ('journalIdGaps', '源 journal 序号断点', '处'), ('sourceHighId', '源 journal 最大 ID', '序号'),
            ('historyWatermark', '历史连续 watermark', '序号')):
            expected_counts = dict(expectedLocal=counts['generated'], sourceRows=projection.get('expectedLocal'),
                historyRows=projection.get('expectedAlarms'),
                sourceHighId=projection.get('expectedLocal'), historyWatermark=projection.get('sourceHighId'))
            actual = projection.get(key)
            expected = expected_counts.get(key, 0)
            decision = '未采集' if actual is None or expected is None else '通过' if actual == expected else '不通过'
            if key == 'expectedAlarms':
                decision = '未采集' if actual is None else '独立 ledger 期望值'
            rows.append((label, actual, decision, unit))
        for key, label in (('projected_through', '源投影确认游标'), ('cleaned_through', '源清理游标')):
            actual = projection.get('sourceCursor', {}).get(key)
            expected = 0 if key == 'cleaned_through' else projection.get('sourceHighId')
            decision = '未采集' if actual is None or expected is None else '通过' if actual == expected else '不通过'
            rows.append((label, actual, decision, '序号'))
        identity = projection.get('actualIdentity', {})
        expected = projection.get('expectedIdentity') or {}
        for key, label in (('storeId', '源 storeId'), ('configGeneration', '源配置代际 configGeneration'),
                           ('journalGeneration', '源 journal 代际 journalGeneration')):
            rows.append((label, identity.get(key), '与启动记录一致' if identity.get(key) is not None and
                         identity.get(key) == expected.get(key) else '未证实一致', '—'))
        rows.append(('历史 journal 代际', projection.get('historyIdentity', {}).get('journal_generation'),
                     '必须等于源 journal 代际', '—'))
        add('源 journal 与历史投影', rows)
    add('版本与证据身份', [
        ('开始时间（UTC）', result.get('startedAt'), '实测记录', '—'),
        ('结束时间（UTC）', result.get('finishedAt'), '实测记录', '—'),
        ('二进制路径', result.get('binary'), '本次输入', '—'),
        ('二进制 SHA256', result.get('binarySha256'), '不等于当前源码已重新编译', '—'),
        ('SQLite 实际版本', result.get('hello', {}).get('sqliteVersion'), '来自被测进程 Hello', '—'),
        ('SQLite 显式库', result.get('sqliteLibrary') or '系统默认解析', '配置路径', '—'),
        ('SQLite 显式库 SHA256', result.get('sqliteLibrarySha256'), '系统默认库未采集哈希', '—'),
        ('存储 profile', config.get('profile'), '配置值', '—'),
    ])
    return tables


def render(output, result):
    conclusions = [verdict_label(result)]
    if result.get('syntheticReportFixture'):
        conclusions.append('本页来自合成夹具，时长和数据库内容可能为构造值；不能作为任何实际长跑证据。')
    elif result.get('reconciliationPassed'):
        conclusions.append(f"已生成 {result['counts']['generated']} 个事件，确认提交 {result['counts']['committed']} 个，"
                           f"确认 Ack {result['counts']['acked']} 个；提交和 Ack 计数均按事件计算。")
    else:
        conclusions.append('当前证据不满足完整对账条件。失败或缺能力没有被降级为通过。')
    if result.get('coverageMode') == 'full':
        conclusions.append('full 源 journal 与历史逐 ID 对账' + ('通过。' if result['journalProjection'].get('passed') else '未通过，真实 full 能力仍需核查。'))
        conclusions.append('同批基线状态 CAS 对账' + ('通过。' if result.get('stateReconciliation', {}).get('passed') else '未通过或未采集，不能认定完整单写链路。'))
    else:
        conclusions.append('本次只覆盖旧 events 投递，不覆盖 localEvents、源 journal 或历史投影。')
    notes = [
        'submitted 为已发起逻辑 Append 中按 eventId 去重的事件数；流水在调用前记录，不证明服务端已接收。',
        'committed 为通过提交回执校验的去重事件数；提交批数单列。同一批重试不增加事件数或批数。',
        '未知响应按每次异常计数，未知请求按逻辑请求计数；一次请求可有多次未知响应，两者不能相加。',
        '重复 RPC 是请求重放，重复 Claim 是重复交付，重复持久化是源或历史表多余行；三者分别列示。',
        '延迟使用精确最近秩分位。提交/Ack 为成功批次样本，端到端为成功事件样本；包括 IPC、重试及证据落盘开销。',
        '资源仅为采样值；未配置延迟和资源合格阈值，因此不能据此判定性能达标。',
        '多个身份由单调度器串行调用，非并发压测。重启在完整 RPC 边界，不覆盖事务中断。',
        '吞吐分母是负载、投递排空与投影排空墙钟之和；排空时长不计入 24 小时负载门槛。',
        'full 每 producer 一个 stateKey；expectedVersion 按已确认 Append 批次递增，重试不推进。状态、localEvents 与 outbox 放入同一个 Append 请求。',
        '本 raw IPC soak 不覆盖 MQTT callback；AckBatch 是合成 sender 的确认。真实 PUBACK 回调链路由 event_store_transport_test 另行测试，不能借本报告代证。',
    ]
    if not result['operations']['available']:
        notes.append('未留存 operations.jsonl，提交与未知/重试计数不可用；不能从 committed 反推 submitted。')
    if result['operations'].get('malformedLines'):
        notes.append('操作流水存在损坏行，相关计数只反映可解析记录，不能视为完整计数。')
    uncovered = ['生产统一单写链路与生产服务验收', '真实 MQTT broker 的 PUBACK', '主机重启与真实断电耐久性',
                 '迁移及回滚', '错误 expectedVersion / 跨 owner 的 CAS 拒绝和事务中断回滚', '并发 IPC 客户端压力', 'SQLite 内部事务 commit 耗时']
    if not result.get('stateReconciliation', {}).get('passed'):
        uncovered.append('baseline states 的版本 CAS 成功路径与最终状态核对')
    if not result.get('accepted24h'):
        uncovered.insert(0, '单次实际 24 小时 full 合格长跑')
    if result.get('coverageMode') != 'full' or not result['journalProjection'].get('passed'):
        uncovered.append('真实 full 链路的完整 journal 与历史投影验收')
    if result.get('syntheticReportFixture'):
        uncovered.insert(0, '本合成夹具不覆盖任何真实 runtime 运行')
    errors = [str(result[key]) for key in ('error', 'cleanupError') if result.get(key)]
    errors.extend(result['journalProjection'].get('errors', []))
    errors.extend(result.get('stateReconciliation', {}).get('errors', []))

    def md_cell(value):
        return html.escape(value_text(value), quote=False).replace('|', '&#124;').replace('\n', '<br>')

    md = ['# EventStore 长跑测试报告', '', '## 关键结论', ''] + ['- ' + md_cell(x) for x in conclusions]
    h = ['<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">',
         '<meta name="viewport" content="width=device-width,initial-scale=1">',
         '<title>EventStore 长跑测试报告</title><style>',
         'body{font:15px/1.6 system-ui,sans-serif;margin:24px auto;padding:0 16px;max-width:1180px;color:#202428;background:#fff}',
         'h1{font-size:26px}h2{font-size:20px;margin-top:28px}table{border-collapse:collapse;width:100%;table-layout:fixed}',
         'th,td{border:1px solid #cbd0d4;padding:8px;text-align:left;vertical-align:top;overflow-wrap:anywhere}',
         'th{background:#eef3f0}th:nth-child(1){width:28%}th:nth-child(2){width:25%}th:nth-child(3){width:32%}',
         'li{overflow-wrap:anywhere}a{color:#16663f}@media(max-width:600px){body{margin:12px auto;padding:0 8px}th,td{padding:5px;font-size:13px}}',
         '</style></head><body><h1>EventStore 长跑测试报告</h1><h2>关键结论</h2><ul>']
    h += ['<li>' + html.escape(x) + '</li>' for x in conclusions]
    h.append('</ul>')
    for title, rows in sections(result):
        md += ['', '## ' + title, '', '| 指标 | 实测值 | 判定 | 单位 |', '| --- | --- | --- | --- |']
        h.append('<section><h2>' + html.escape(title) + '</h2><table><thead><tr>' +
                 ''.join('<th scope="col">' + name + '</th>' for name in ('指标', '实测值', '判定', '单位')) + '</tr></thead><tbody>')
        for row in rows:
            md.append('| ' + ' | '.join(md_cell(cell) for cell in row) + ' |')
            h.append('<tr>' + ''.join('<td>' + html.escape(value_text(cell)) + '</td>' for cell in row) + '</tr>')
        h.append('</tbody></table></section>')
    for title, items in (('计量口径与限制', notes), ('未覆盖项', uncovered), ('异常诊断', errors or ['未记录运行错误；仍须结合上表缺测与覆盖限制判断。'])):
        md += ['', '## ' + title, ''] + ['- ' + md_cell(item) for item in items]
        h.append('<h2>' + title + '</h2><ul>' + ''.join('<li>' + html.escape(item) + '</li>' for item in items) + '</ul>')
    links = [('report.json', '机器解析 JSON'), ('events.csv', '逐事件 CSV'), ('resources.csv', '资源 CSV'),
             ('anomalies.csv', '异常 CSV'), ('operations.jsonl', '原始操作流水'), ('server.log', '被测进程日志')]
    if result.get('coverageMode') == 'full':
        links.append(('journal_reconciliation.csv', 'journal 与历史逐 ID 对账 CSV'))
        links.append(('state_reconciliation.csv', '逐 producer 基线状态 CAS 对账 CSV'))
    links = [(name, label) for name, label in links if (output / name).is_file()]
    md += ['', '## 原始证据', ''] + [f'- [{label}]({name})' for name, label in links]
    h.append('<h2>原始证据</h2><ul>' + ''.join(f'<li><a href="{name}">{label}</a></li>' for name, label in links) + '</ul></body></html>')
    (output / 'report.md').write_text('\n'.join(md) + '\n', encoding='utf-8')
    (output / 'report.html').write_text('\n'.join(h) + '\n', encoding='utf-8')
