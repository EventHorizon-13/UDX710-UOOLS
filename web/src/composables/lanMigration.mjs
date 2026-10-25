export function migrationAddressUrl(address) {
  const parts = String(address).split('.')
  if (parts.length !== 4 || parts.some(p => !/^(0|[1-9]\d{0,2})$/.test(p) || Number(p) > 255)) {
    throw new Error('Invalid LAN address')
  }
  return `http://${address}:6677/?lan=confirm`
}

// Only the authenticated confirmation POST from the new socket makes an
// applied address durable. Polling the old address cannot commit it.
export function createMigrationCompletion({ confirm, navigate, onConfirmed, onError }) {
  const attempted = new Set()
  const redirected = new Set()
  return async function settle(state, { redirect = false, force = false } = {}) {
    const migration = state?.migration
    if (!migration || state.migration_error || migration.seconds_left <= 0) return false
    if (migration.confirmable) {
      if (attempted.has(migration.id) && !force) return false
      attempted.add(migration.id)
      try {
        await confirm(migration.id)
        onConfirmed()
        return true
      } catch (error) {
        onError(error)
        return false
      }
    }
    if (redirect && !redirected.has(migration.id)) {
      const url = migrationAddressUrl(migration.address)
      redirected.add(migration.id)
      navigate(url)
    }
    return false
  }
}
