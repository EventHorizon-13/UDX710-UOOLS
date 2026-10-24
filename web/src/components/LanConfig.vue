<script setup>
import { ref, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { authFetch } from '../composables/useApi'
import { useToast } from '../composables/useToast'

const { t } = useI18n()
const { success, error: showError } = useToast()
const state = ref(null)
const start = ref('')
const end = ref('')
const busy = ref(false)

async function load() {
  busy.value = true
  try {
    const response = await authFetch('/api/lan')
    const result = await response.json()
    if (!response.ok || result.Code !== 0) throw new Error(result.error || result.Error || response.status)
    state.value = result.Data
    start.value = result.Data.pool_start || ''
    end.value = result.Data.pool_end || ''
  } catch (err) {
    showError(t('lan.loadError') + ': ' + err.message)
  } finally { busy.value = false }
}

async function save() {
  if (!window.confirm(t('lan.confirm'))) return
  busy.value = true
  try {
    const response = await authFetch('/api/lan', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ pool_start: start.value.trim(), pool_end: end.value.trim(), version: state.value.version, confirm: true })
    })
    const result = await response.json()
    if (!response.ok || result.Code !== 0) throw new Error(result.error || result.Error || response.status)
    success(t('lan.saved'))
    await load()
  } catch (err) {
    showError(t('lan.saveError') + ': ' + err.message)
  } finally { busy.value = false }
}

async function toggleDhcp() {
  const enabled = !state.value.dhcp.enabled
  if (!window.confirm(t(enabled ? 'lan.confirmDhcpOn' : 'lan.confirmDhcpOff'))) return
  busy.value = true
  try {
    const response = await authFetch('/api/lan', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ action: 'dhcp', enabled, version: state.value.dhcp.version, confirm: true })
    })
    const result = await response.json()
    if (!response.ok || result.Code !== 0) throw new Error(result.error || result.Error || response.status)
    success(t(enabled ? 'lan.dhcpOn' : 'lan.dhcpOff'))
    await load()
  } catch (err) {
    showError(t('lan.saveError') + ': ' + err.message)
    await load()
  } finally { busy.value = false }
}

onMounted(load)
</script>

<template>
  <section class="lan-panel">
    <header><h3>LAN</h3><button type="button" @click="load" :disabled="busy">{{ t('wifi.refresh') }}</button></header>
    <p v-if="state && !state.supported" role="status">{{ t('lan.unsupported') }}</p>
    <template v-else-if="state?.supported">
      <dl>
        <div><dt>IPv4</dt><dd>{{ state.address }}/{{ state.prefix }}</dd></div>
        <div><dt>{{ t('lan.netmask') }}</dt><dd>{{ state.netmask }}</dd></div>
        <div><dt>{{ t('lan.dns') }}</dt><dd><span v-for="ip in state.dns" :key="ip">{{ ip }}</span><span v-if="!state.dns?.length">—</span></dd></div>
      </dl>
      <div class="dhcp-row">
        <div><span class="dhcp-label">{{ t('lan.dhcp') }}</span><p>{{ t('lan.dhcpScope') }}</p></div>
        <button type="button" class="dhcp-switch" role="switch" :aria-label="t('lan.dhcp')"
          :aria-checked="state.dhcp?.supported ? state.dhcp.enabled : false"
          :class="{ on: state.dhcp?.supported && state.dhcp.enabled }"
          :disabled="busy || !state.dhcp?.supported || !state.dhcp.consistent" @click="toggleDhcp">
          <span></span>
        </button>
      </div>
      <p v-if="!state.dhcp?.supported || !state.dhcp.consistent" class="dhcp-warning" role="status">{{ t('lan.dhcpUnavailable') }}</p>
      <p v-else-if="!state.dhcp.enabled" class="notice dhcp-off" role="status">{{ t('lan.dhcpOffNotice') }}</p>
      <form @submit.prevent="save">
        <label>{{ t('lan.poolStart') }}<input v-model="start" required inputmode="decimal" autocomplete="off" :disabled="busy || !state.pool_writable" /></label>
        <label>{{ t('lan.poolEnd') }}<input v-model="end" required inputmode="decimal" autocomplete="off" :disabled="busy || !state.pool_writable" /></label>
        <button type="submit" class="save" :disabled="busy || !state.pool_writable || (start === state.pool_start && end === state.pool_end)">{{ busy ? t('common.saving') : t('wifi.saveConfig') }}</button>
      </form>
      <p class="notice">{{ t('lan.addressWarning') }}</p>
    </template>
  </section>
</template>

<style scoped>
.lan-panel { color:inherit; background:var(--wifi-surface); border: 1px solid rgb(148 163 184 / .25); border-radius: 16px; padding: 16px; }
header { display:flex; align-items:center; justify-content:space-between; gap:12px; margin-bottom:12px; }
h3 { font-size:16px; font-weight:700; }
button { border:1px solid rgb(148 163 184 / .4); border-radius:8px; padding:7px 12px; font-size:13px; }
button:disabled { opacity:.45; cursor:default; }
button:focus-visible, input:focus-visible { outline:2px solid #38bdf8; outline-offset:2px; }
dl { display:grid; grid-template-columns:repeat(auto-fit,minmax(180px,1fr)); gap:12px; font-size:13px; margin-bottom:14px; }
dt { color:var(--wifi-muted); margin-bottom:3px; } dd { overflow-wrap:anywhere; } dd span { display:block; }
form { display:flex; flex-wrap:wrap; align-items:end; gap:12px; }
label { display:grid; flex:1 1 180px; gap:5px; font-size:13px; }
input { width:100%; min-width:0; background:transparent; border:1px solid rgb(148 163 184 / .4); border-radius:8px; padding:8px 10px; }
.save { background:#0284c7; color:white; padding:9px 14px; }
.dhcp-row { display:flex; align-items:center; justify-content:space-between; gap:16px; margin:4px 0 14px; }
.dhcp-label { font-size:14px; font-weight:600; }
.dhcp-row p { margin:3px 0 0; color:var(--wifi-muted); font-size:12px; line-height:1.5; }
button.dhcp-switch { position:relative; flex:0 0 48px; width:48px; height:26px; min-height:26px; padding:0; border:0; border-radius:999px; background:#64748b; transition:background .18s ease; }
button.dhcp-switch.on { background:#0284c7; }
.dhcp-switch span { display:block; position:absolute; top:3px; left:3px; width:20px; height:20px; border-radius:50%; background:white; box-shadow:0 1px 3px rgb(0 0 0 / .18); transition:transform .18s ease; }
.dhcp-switch.on span { transform:translateX(22px); }
.dhcp-warning { color:#d97706; font-size:12px; margin-bottom:12px; }
.notice.dhcp-off { margin:0 0 12px; }
@media (prefers-reduced-motion:reduce) { button.dhcp-switch,.dhcp-switch span { transition:none; } }
.notice { font-size:12px; color:var(--wifi-muted); margin-top:12px; line-height:1.6; }
</style>
