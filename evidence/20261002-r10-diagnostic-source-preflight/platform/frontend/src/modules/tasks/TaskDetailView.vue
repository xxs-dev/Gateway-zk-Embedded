<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import { useAuthStore } from '@/modules/auth/auth-store'
import AsyncState from '@/shared/components/AsyncState.vue'
import StatusBadge from '@/shared/components/StatusBadge.vue'
import { formatDateTime } from '@/shared/utils/format'
import { cancelTask, getTask } from './api'
import type { TaskDetail } from './types'

const route = useRoute()
const auth = useAuthStore()
const taskId = computed(() => String(route.params.taskId ?? ''))
const detail = ref<TaskDetail | null>(null)
const loading = ref(false)
const cancelling = ref(false)
const error = ref('')
let controller: AbortController | null = null
let pollTimer: number | undefined

const terminalStatuses = new Set(['SUCCEEDED', 'FAILED', 'TIMED_OUT', 'CANCELLED'])
const primaryTarget = computed(() => detail.value?.targets[0] ?? null)
const configTask = computed(() => detail.value?.summary.type === 'CONFIG_PUBLISH'
  || detail.value?.summary.type === 'CONFIG_ROLLBACK')
const backLink = computed(() => configTask.value && detail.value?.parameters.revisionId
  ? `/assets/config-revisions/${encodeURIComponent(detail.value.parameters.revisionId)}`
  : primaryTarget.value
    ? `/fleet/${encodeURIComponent(primaryTarget.value.machineCode)}/tasks`
    : '/tasks')

watch(taskId, () => void load(), { immediate: true })

onBeforeUnmount(() => {
  controller?.abort()
  if (pollTimer) window.clearTimeout(pollTimer)
})

async function load(): Promise<void> {
  controller?.abort()
  controller = new AbortController()
  loading.value = detail.value === null
  error.value = ''
  try {
    detail.value = await getTask(taskId.value, controller.signal)
  } catch (caught) {
    if (!(caught instanceof DOMException && caught.name === 'AbortError')) {
      error.value = caught instanceof Error ? caught.message : '任务详情加载失败'
    }
  } finally {
    loading.value = false
    scheduleRefresh()
  }
}

function scheduleRefresh(): void {
  if (pollTimer) window.clearTimeout(pollTimer)
  if (detail.value && !terminalStatuses.has(detail.value.summary.status)) {
    pollTimer = window.setTimeout(() => void load(), 3_000)
  }
}

async function cancel(): Promise<void> {
  if (!detail.value || !window.confirm('确认在投递前取消此任务吗？')) return
  cancelling.value = true
  error.value = ''
  try {
    await cancelTask(detail.value.summary.id)
    await load()
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : '取消任务失败'
  } finally {
    cancelling.value = false
  }
}
</script>

<template>
  <AsyncState :loading="loading" :error="error" :empty="!detail" empty-title="任务不存在"
    empty-description="该任务不存在，或不在你的数据权限范围内。" @retry="load">
    <template v-if="detail">
      <div class="task-detail-actions">
        <RouterLink class="text-button" :to="backLink">← 返回任务列表</RouterLink>
        <button v-if="auth.has('task:create') && detail.allowedActions.includes('CANCEL')" type="button"
          class="secondary-button" :disabled="cancelling" @click="cancel">
          {{ cancelling ? '取消中…' : '取消任务' }}
        </button>
      </div>

      <section class="surface task-summary-card">
        <div class="task-summary-title">
          <div>
            <p class="eyebrow">OPERATION TASK</p>
            <h2>{{ detail.summary.title }}</h2>
            <p>{{ detail.summary.id }}</p>
          </div>
          <StatusBadge :status="detail.summary.status" />
        </div>
        <dl class="task-facts">
          <div><dt>任务类型</dt><dd>{{ detail.summary.type }}</dd></div>
          <div><dt>风险等级</dt><dd>{{ detail.summary.riskLevel }}</dd></div>
          <div><dt>发起人</dt><dd>{{ detail.summary.requestedBy }}</dd></div>
          <div><dt>创建时间</dt><dd>{{ formatDateTime(detail.summary.createdAt) }}</dd></div>
          <div><dt>{{ configTask ? '配置版本' : '诊断动作' }}</dt><dd>{{ configTask ? detail.parameters.version : detail.parameters.commandName ?? '—' }}</dd></div>
          <div><dt>尝试次数</dt><dd>{{ primaryTarget?.attempt ?? 0 }}</dd></div>
        </dl>
        <p v-if="detail.summary.failureMessage" class="task-failure">
          {{ detail.summary.failureMessage }}（{{ detail.summary.failureCode }}）
          <span v-if="detail.summary.retryable">可在确认链路恢复后重新创建任务。</span>
        </p>
      </section>

      <div class="task-detail-grid">
        <section class="surface task-result-card">
          <div class="section-heading">
            <div><p class="eyebrow">TARGET RESULT</p><h3>设备执行结果</h3></div>
            <StatusBadge v-if="primaryTarget" :status="primaryTarget.status" />
          </div>
          <template v-if="primaryTarget">
            <dl class="result-facts">
              <div><dt>目标</dt><dd>{{ primaryTarget.deviceName }} · {{ primaryTarget.machineCode }}</dd></div>
              <div><dt>结果码</dt><dd>{{ primaryTarget.resultCode ?? '等待结果' }}</dd></div>
              <div><dt>截止时间</dt><dd>{{ formatDateTime(primaryTarget.deadlineAt) }}</dd></div>
            </dl>
            <template v-if="configTask">
              <div class="ota-progress-card">
                <div><span>边缘阶段</span><strong>{{ primaryTarget.result.stage ?? '等待边缘回执' }}</strong></div>
                <div><span>执行进度</span><strong>{{ primaryTarget.progress }}%</strong></div>
                <div><span>回执消息</span><strong>{{ primaryTarget.result.message ?? '暂无' }}</strong></div>
              </div>
              <RouterLink v-if="detail.parameters.revisionId" class="row-link config-task-revision-link"
                :to="{ name: 'config-revision-detail', params: { revisionId: detail.parameters.revisionId } }">
                查看关联配置版本 →
              </RouterLink>
            </template>
            <p v-else-if="primaryTarget.result.redacted || primaryTarget.result.truncated" class="result-notice">
              输出已由平台{{ primaryTarget.result.redacted ? '脱敏' : '' }}{{ primaryTarget.result.redacted && primaryTarget.result.truncated ? '并' : '' }}{{ primaryTarget.result.truncated ? '截断' : '' }}后存储。
            </p>
            <template v-if="!configTask">
              <dl v-if="primaryTarget.result.reason !== undefined || primaryTarget.result.cleanupConfirmed !== undefined" class="result-facts">
                <div><dt>设备分类</dt><dd>{{ primaryTarget.result.reason ?? '未知' }}</dd></div>
                <div><dt>信号</dt><dd>{{ primaryTarget.result.signal ?? '未知' }}</dd></div>
                <div><dt>设备超时</dt><dd>{{ primaryTarget.result.timedOut === undefined ? '未知' : primaryTarget.result.timedOut ? '是' : '否' }}</dd></div>
                <div><dt>清理确认</dt><dd>{{ primaryTarget.result.cleanupConfirmed === undefined ? '未知' : primaryTarget.result.cleanupConfirmed ? '是' : '否' }}</dd></div>
              </dl>
              <h4>标准输出</h4>
              <pre class="task-output">{{ primaryTarget.result.stdout || '暂无标准输出' }}</pre>
              <h4>错误输出</h4>
              <pre class="task-output error-output">{{ primaryTarget.result.stderr || '暂无错误输出' }}</pre>
            </template>
          </template>
        </section>

        <section class="surface task-timeline-card">
          <div class="section-heading"><div><p class="eyebrow">AUDIT TIMELINE</p><h3>生命周期</h3></div></div>
          <ol class="task-timeline">
            <li v-for="event in detail.events" :key="event.id">
              <i aria-hidden="true" />
              <div>
                <strong>{{ event.message }}</strong>
                <span>{{ event.actorName ?? '系统' }} · {{ formatDateTime(event.occurredAt) }}</span>
                <small v-if="event.toStatus">{{ event.fromStatus ?? '—' }} → {{ event.toStatus }}</small>
              </div>
            </li>
          </ol>
          <p class="protocol-note">{{ configTask
            ? '配置任务通过 MQTT 接收 accepted、下载、校验、应用和 completed；completed 表示应用脚本完成，尚不等于独立健康探测通过。'
            : '协议说明：边端诊断当前没有独立 ACK/进度事件，所以“已投递”后直接等待真实成功、失败或超时终态。' }}</p>
        </section>
      </div>
    </template>
  </AsyncState>
</template>
