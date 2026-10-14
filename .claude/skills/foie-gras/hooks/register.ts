import type { Register } from 'claude-code'

// "feed: <task>" or "feed:NAME <task>" reaches Claude as just <task>, and its turn opens with Read calls for the
// list's files, answered here instead of the first model request. Claude Code runs them with its Read tool, so
// Claude continues from reads of its own: the files count as read, and nothing is left to reread.
const FEED = /^\s*feed:(\S*)(?:\s+([\s\S]*))?$/
const ID = 'toolu_foiegras'
const TYPED = new Set(['composer', 'sdk', 'bridge'])
let pending: string[] = []

export const register: Register = on => {
  on('prompt.submit', async ($, e, next) => {
    if (!TYPED.has(e.origin.kind)) return next(e)
    const feed = FEED.exec(e.text)
    if (!feed) return e.text.trim() === 'feed' ? { drop: 'foie_gras: put your task after feed: or feed:NAME' } : next(e)
    const [, name, task] = feed
    if (!task?.trim()) return { drop: `foie_gras: put your task after feed:${name}` }
    if (e.turnId !== undefined) return { drop: 'foie_gras: send feed: once Claude has finished' }
    const ran = await $.process.run(['python3', `${$.plugin.root}/feed.py`, `--files=${name || 'default'}`])
    if (ran.exitCode !== 0) return { drop: ran.stderr.trim() }
    pending = ran.stdout.split('\n').filter(Boolean)
    const entered = await next({ ...e, text: task })
    if (entered.drop !== undefined) pending = []
    return entered
  }).catch(($, e, next) => (next.called ? next(e) : { drop: 'foie_gras: the hook failed, see claude --debug' }))

  on('turn.step', async function* ($, e, next) {
    if (e.index !== 0 || e.agentId !== undefined || !pending.length) return yield* next(e)
    const inputs = pending.map(file_path => ({ file_path, allow_large: true }))
    pending = []
    const run = Date.now().toString(36)
    for (const [index, input] of inputs.entries()) {
      yield { kind: 'tool', index, id: `${ID}_${run}_${index}`, name: 'Read' }
      yield { kind: 'input', index, json: JSON.stringify(input) }
    }
    yield { kind: 'stop', stopReason: 'tool_use', usage: null }
    const toolUses = inputs.map(input => ({ name: 'Read', input }))
    return { turnId: e.turnId, index: e.index, answer: '', toolUses, stopReason: 'tool_use', usage: null }
  })
}
