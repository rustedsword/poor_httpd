import type { Plugin } from '@opencode/plugin'
import { execFile } from 'node:child_process'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

// "feed: <task>" or "feed:NAME <task>" reaches the model as just <task>, with the list's files attached. Every request
// then shows the model those files as read calls of its own, so it takes them as read instead of reading them again.
const FEED = /^\s*feed:(\S*)(?:\s+([\s\S]*))?$/
const SCRIPT = fileURLToPath(new URL('../foie_gras.py', import.meta.url))

const listed = (cwd: string, name: string) =>
  new Promise<string[]>((resolve, reject) =>
    execFile('python3', [SCRIPT, `--files=${name}`], { cwd }, (error, stdout, stderr) =>
      error ? reject(new Error(stderr.trim() || `foie_gras: ${error.message}`)) : resolve(stdout.split('\n').filter(Boolean)),
    ),
  )

// What the read tool returns for a whole file.
const read = (name: string, text: string) => {
  const lines = text.replace(/\n$/, '').split('\n')
  return [`Read file ${name}, lines 1-${lines.length}`, ...lines.map((line, index) => `${index + 1}: ${line}`)].join('\n')
}

export default {
  id: 'foie-gras',
  setup: async ctx => {
    await ctx.session.hook('prompt', async event => {
      const { prompt } = event
      // `opencode run` quotes each message argument that contains spaces, and appends piped input on a new line.
      const quoted = /^"((?:\\"|[^"])*)"/.exec(prompt.text)
      const first = quoted?.[1].replaceAll('\\"', '"')
      const text = first !== undefined && /^\s*feed(?::|\s*$)/.test(first) ? first + prompt.text.slice(quoted![0].length) : prompt.text
      // A hook that throws fails the prompt with a bare 500. An attachment that is not a URL fails it with this text.
      const refuse = (message: string) => void (prompt.files = [{ uri: message }])
      const feed = FEED.exec(text)
      if (!feed) return text.trim() === 'feed' ? refuse('foie_gras: put your task after feed: or feed:NAME') : undefined
      const [, name, task] = feed
      if (!task?.trim()) return refuse(`foie_gras: put your task after feed:${name}`)
      const files = await listed(ctx.location.directory, name || 'default').catch((error: Error) => error)
      if (files instanceof Error) return refuse(files.message)
      const attached = files.map(file => ({ uri: pathToFileURL(file).href, name: path.relative(ctx.location.directory, file) }))
      event.metadata = { ...event.metadata, foie_gras: attached.map(file => file.uri) }
      prompt.text = task
      prompt.files = [...attached, ...(prompt.files ?? [])]
    })

    // The session keeps the files as attachments; each request swaps them for read calls and their results.
    await ctx.session.hook('context', event => {
      event.messages.splice(0, event.messages.length, ...event.messages.flatMap(message => {
        const fed = new Set(message.role === 'user' ? (message.metadata?.foie_gras as string[] | undefined) : undefined)
        const reads = message.content.flatMap((part, index) => {
          const attachment = part.metadata?.attachment as { source?: { uri?: string }; name?: string } | undefined
          const header = `\n\nAttached file: ${attachment?.name}\n\n`
          if (part.type !== 'text' || !fed.has(attachment?.source?.uri) || !part.text.startsWith(header)) return []
          return [{ part, id: `fg_${String(message.id).slice(-12)}_${index}`, name: attachment!.name!, text: part.text.slice(header.length) }]
        })
        if (!reads.length) return [message]
        const Message = message.constructor as new (fields: object) => typeof message
        return [
          new Message({ ...message, content: message.content.filter(part => !reads.some(item => item.part === part)) }),
          new Message({ role: 'assistant', content: reads.map(({ id, name }) => ({ type: 'tool-call', id, name: 'read', input: { path: name } })) }),
          ...reads.map(({ id, name, text }) =>
            new Message({ role: 'tool', content: [{ type: 'tool-result', id, name: 'read', result: { type: 'text', value: read(name, text) } }] })),
        ]
      }))
    })
  },
} satisfies Plugin.Plugin
