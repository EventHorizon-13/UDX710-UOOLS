import test from 'node:test'
import assert from 'node:assert/strict'
import { createMigrationCompletion, migrationAddressUrl } from '../web/src/composables/lanMigration.mjs'

const pending = confirmable => ({ migration: { id: 'lan-address-example', address: '192.168.1.1', seconds_left: 100, confirmable } })
function fixture(confirm = async () => {}) {
  const events = []
  const settle = createMigrationCompletion({
    confirm: async id => { events.push(['confirm', id]); await confirm(id) },
    navigate: url => events.push(['navigate', url]),
    onConfirmed: () => events.push(['saved']),
    onError: error => events.push(['error', error.message])
  })
  return { events, settle }
}
test('apply from the old address navigates to the new address without claiming success', async () => {
  const { events, settle } = fixture()
  assert.equal(await settle(pending(false), { redirect: true }), false)
  assert.deepEqual(events, [['navigate', 'http://192.168.1.1:6677/?lan=confirm']])
  await settle(pending(false), { redirect: true })
  assert.equal(events.length, 1)
})
test('new-address login commits automatically; success waits for the confirmation response', async () => {
  let resolve
  const { events, settle } = fixture(() => new Promise(r => { resolve = r }))
  const result = settle(pending(true))
  assert.deepEqual(events, [['confirm', 'lan-address-example']])
  await settle(pending(true))
  assert.equal(events.length, 1)
  resolve()
  assert.equal(await result, true)
  assert.deepEqual(events.at(-1), ['saved'])
})
test('failed confirmation never reports saved and remains manually retryable', async () => {
  let fail = true
  const { events, settle } = fixture(async () => { if (fail) throw new Error('401') })
  assert.equal(await settle(pending(true)), false)
  assert.deepEqual(events.at(-1), ['error', '401'])
  await settle(pending(true))
  assert.equal(events.length, 2)
  fail = false
  assert.equal(await settle(pending(true), { force: true }), true)
  assert.deepEqual(events.at(-1), ['saved'])
})
test('old-address polling, expired transactions and rollback errors cannot confirm', async () => {
  const { events, settle } = fixture()
  for (const state of [pending(false), { migration: null }, { ...pending(true), migration_error: true }, { migration: { ...pending(true).migration, seconds_left: 0 } }]) {
    assert.equal(await settle(state), false)
  }
  assert.deepEqual(events, [])
})
test('redirects contain only a canonical device address and no credentials', () => {
  for (const value of ['192.168.1.1@evil.example', '192.168.1.1/evil', '192.168.001.1', '256.1.1.1', 'javascript:alert(1)']) {
    assert.throws(() => migrationAddressUrl(value))
  }
  const url = new URL(migrationAddressUrl('192.168.1.1'))
  assert.equal(url.username + url.password + url.hash, '')
  assert.equal(url.search, '?lan=confirm')
})
