export type TaskStatus =
  | 'CREATED'
  | 'QUEUED'
  | 'DISPATCHING'
  | 'DISPATCHED'
  | 'ACKED'
  | 'RUNNING'
  | 'SUCCEEDED'
  | 'FAILED'
  | 'TIMED_OUT'
  | 'CANCELLED'

export interface TaskSummary {
  id: string
  type: string
  riskLevel: string
  status: TaskStatus
  title: string
  requestedBy: string
  targetCount: number
  succeededCount: number
  failedCount: number
  failureCode: string | null
  failureMessage: string | null
  retryable: boolean
  createdAt: string
  updatedAt: string
  finishedAt: string | null
}

export interface TaskPage {
  items: TaskSummary[]
  nextCursor: string | null
  pageSize: number
}

export interface TaskTargetResult {
  exitCode?: number
  stdout?: string
  stderr?: string
  requestTs?: number
  replyTs?: number
  redacted?: boolean
  truncated?: boolean
  reason?: string
  signal?: number
  timedOut?: boolean
  cleanupConfirmed?: boolean
  stage?: string
  progress?: number
  message?: string
  downloadedBytes?: number
  totalBytes?: number
  ts?: number
}

export interface TaskTarget {
  id: number
  machineCode: string
  deviceName: string
  status: TaskStatus
  progress: number
  attempt: number
  resultCode: string | null
  result: TaskTargetResult
  dispatchedAt: string | null
  deadlineAt: string | null
  finishedAt: string | null
}

export interface TaskEvent {
  id: number
  targetId: number | null
  eventType: string
  fromStatus: TaskStatus | null
  toStatus: TaskStatus | null
  message: string
  payload: Record<string, unknown>
  actorName: string | null
  occurredAt: string
}

export interface TaskDetail {
  summary: TaskSummary
  parameters: {
    commandName?: string
    service?: string
    tailLines?: number
    revisionId?: string
    version?: string
  }
  targets: TaskTarget[]
  events: TaskEvent[]
  allowedActions: string[]
}

export interface RemoteDiagnosticRequest {
  machineCode: string
  commandName: string
  service?: string
  tailLines: number
}

export interface DiagnosticCapabilities {
  allowedCommands: string[]
  minTailLines: number
  maxTailLines: number
  channelReady: boolean
}

export interface DiagnosticPrecheck {
  allowed: boolean
  blockingCode: string | null
  machineCode: string
  deviceName: string
  riskLevel: string
  warnings: string[]
}

export interface CreateTaskResult {
  id: string
  status: TaskStatus
  created: boolean
}
