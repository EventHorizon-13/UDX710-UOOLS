<script setup>
import { ref, computed, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { authFetch } from '../composables/useApi'
import { useToast } from '../composables/useToast'
import LanConfig from './LanConfig.vue'

const { t } = useI18n()
const { success, error: showError } = useToast()
const bands = ref([])
const clients = computed(() => bands.value.flatMap(band => (band.clients || []).map(client => ({
  ...client,
  interface: band.interface,
  band: band.interface === 'wlan1' ? '2.4G' : '5G'
}))))
const clientsComplete = computed(() => bands.value.every(band => band.clients_complete !== false))
const clientWarnings = computed(() => bands.value.flatMap(band => (band.client_warnings || []).map(warning => `${band.band}: ${warning}`)))
const loading = ref(true)
const busyBand = ref('')
const supported = ref(true)
const configOpen = ref({})
const configDraft = ref({})
const channels24 = Array.from({ length: 13 }, (_, i) => i + 1)
const channels5 = [36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 149, 153, 157, 161, 165]

async function loadStatus() {
  loading.value = true
  try {
    const response = await authFetch('/api/wifi')
    const result = await response.json()
    if (!response.ok || result.Code !== 0) throw new Error(result.Error || t('wifi.loadError'))
    supported.value = result.Data?.supported === true
    bands.value = (result.Data?.bands || []).filter(band => band.available)
  } catch (err) {
    showError(t('wifi.loadError') + ': ' + err.message)
  } finally {
    loading.value = false
  }
}

function editConfig(band) {
  configDraft.value[band.interface] = {
    ssid: band.ssid || '',
    password: '',
    channel: Number(band.channel) || (band.interface === 'wlan1' ? 1 : 36),
    hidden: band.hidden === true
  }
  configOpen.value[band.interface] = true
}

async function saveConfig(band) {
  const draft = configDraft.value[band.interface]
  if (!draft?.ssid.trim()) {
    showError(t('wifi.ssidRequired'))
    return
  }
  if (!window.confirm(t('wifi.confirmConfigure'))) return
  busyBand.value = band.interface
  try {
    const response = await authFetch('/api/wifi', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        action: 'configure',
        band: band.interface === 'wlan1' ? '2g' : '5g',
        ssid: draft.ssid.trim(),
        password: draft.password,
        channel: Number(draft.channel),
        hidden: draft.hidden
      })
    })
    const result = await response.json()
    if (!response.ok || result.Code !== 0) throw new Error(result.Error || t('wifi.configureError'))
    success(result.Data || t('wifi.settingsSaved'))
    configOpen.value[band.interface] = false
    await loadStatus()
  } catch (err) {
    showError(t('wifi.configureError') + ': ' + err.message)
  } finally {
    busyBand.value = ''
  }
}

async function setRadio(band) {
  const next = !band.enabled
  if (!window.confirm(next ? t('wifi.confirmEnable') : t('wifi.confirmDisable'))) return
  busyBand.value = band.interface
  try {
    const response = await authFetch('/api/wifi', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ band: band.interface === 'wlan1' ? '2g' : '5g', enabled: next })
    })
    const result = await response.json()
    if (!response.ok || result.Code !== 0) throw new Error(result.Error || t('wifi.loadError'))
    success(result.Data || (next ? t('wifi.enabled') : t('wifi.disabled')))
    await loadStatus()
  } catch (err) {
    showError(err.message)
  } finally {
    busyBand.value = ''
  }
}

onMounted(loadStatus)
</script>

<template>
  <section class="wifi-page">
    <div class="toolbar"><button @click="loadStatus" :disabled="loading"><font-awesome-icon icon="sync-alt" :class="{ 'animate-spin': loading }" /> {{ t('wifi.refresh') }}</button></div>
    <p v-if="loading && !bands.length" role="status">{{ t('common.loading') }}</p>
    <p v-else-if="!supported" role="status">{{ t('wifi.unsupported') }}</p>
    <div v-else class="bands">
      <article v-for="band in bands" :key="band.interface" class="wifi-card">
        <header class="band-header">
          <div class="band-name"><h3>{{ band.band }}</h3><p>{{ band.ssid || '—' }}</p></div>
          <div class="actions">
            <button @click="editConfig(band)" :disabled="busyBand === band.interface">{{ t('wifi.editConfig') }}</button>
            <button type="button" role="switch" class="radio-switch" @click="setRadio(band)" :disabled="loading || !!busyBand" :aria-label="band.band + ' Wi-Fi'" :aria-checked="band.enabled" :aria-busy="busyBand === band.interface"><span class="switch-thumb" /></button>
          </div>
        </header>
        <p class="band-state">{{ t('wifi.channel') }} {{ band.channel || '—' }}<span v-if="band.hidden"> · {{ t('wifi.hidden') }}</span></p>
        <form v-if="configOpen[band.interface]" class="config-form" @submit.prevent="saveConfig(band)">
          <label>{{ t('wifi.ssid') }}<input v-model="configDraft[band.interface].ssid" required maxlength="32" autocomplete="off" /></label>
          <label>{{ t('wifi.newPassword') }}<input v-model="configDraft[band.interface].password" type="password" minlength="8" maxlength="63" autocomplete="new-password" :placeholder="t('wifi.passwordHint')" /></label>
          <label>{{ t('wifi.channel') }}<select v-model.number="configDraft[band.interface].channel"><option v-for="channel in (band.interface === 'wlan1' ? channels24 : channels5)" :key="channel" :value="channel">{{ channel }}</option></select></label>
          <label class="hidden-control"><input v-model="configDraft[band.interface].hidden" type="checkbox" />{{ t('wifi.hidden') }}</label>
          <div class="form-actions"><button type="button" @click="configOpen[band.interface] = false">{{ t('common.cancel') }}</button><button type="submit" :disabled="busyBand === band.interface">{{ busyBand === band.interface ? t('common.saving') : t('wifi.saveConfig') }}</button></div>
        </form>

      </article>
    </div>
    <LanConfig />
    <article v-if="supported && bands.length" class="wifi-card clients-card">
      <h3>{{ t('wifi.clients') }} <span class="client-count">{{ clientsComplete ? clients.length : '—' }}</span></h3>
      <p v-if="clientWarnings.length" class="client-warning" role="status">{{ t('wifi.clientWarning') }}: {{ clientWarnings.join('; ') }}</p>
      <div v-for="client in clients" :key="client.interface + ':' + client.mac" class="client">
        <header class="client-header"><h4>{{ client.hostname || '—' }}</h4><span class="client-band">{{ client.band }}</span></header>
        <dl>
          <div><dt>MAC</dt><dd class="address">{{ client.mac }}</dd></div>
          <div><dt>IPv4</dt><dd class="address"><span v-for="ip in client.ipv4" :key="ip">{{ ip }}</span><span v-if="!client.ipv4?.length">—</span></dd></div>
          <div><dt>IPv6</dt><dd class="address"><span v-for="ip in client.ipv6" :key="ip">{{ ip }}</span><span v-if="!client.ipv6?.length">—</span></dd></div>
        </dl>
      </div>
      <p v-if="!clients.length && clientsComplete" class="empty">{{ t('wifi.noClients') }}</p>
    </article>
  </section>
</template>

<style scoped>
.wifi-page { color:#1e293b; --wifi-surface:rgb(255 255 255 / .85); --wifi-muted:#475569; display:grid; gap:16px; min-width:0; }
:global(html.dark .wifi-page) { color:#f1f5f9; color-scheme:dark; --wifi-surface:#1e293b; --wifi-muted:#cbd5e1; }
.toolbar { display:flex; justify-content:flex-end; }
.bands { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:16px; align-items:start; }
.wifi-card { min-width:0; border:1px solid rgb(148 163 184 / .25); border-radius:16px; padding:16px; background:var(--wifi-surface); }
.band-header { display:flex; flex-wrap:wrap; align-items:start; justify-content:space-between; gap:12px; }
.band-name { min-width:0; flex:1 1 100px; overflow-wrap:anywhere; }
h3 { font-size:16px; font-weight:700; } .band-name p { font-size:14px; margin-top:3px; }
.actions, .form-actions { display:flex; flex-wrap:wrap; align-items:center; gap:12px; }
button { border:1px solid rgb(148 163 184 / .4); border-radius:8px; padding:7px 10px; font-size:13px; transition:background .15s; }
button:hover:enabled { background:rgb(148 163 184 / .12); } button:disabled { opacity:.45; cursor:default; }
button.radio-switch { position:relative; flex:0 0 56px; width:56px; height:28px; padding:0; border:0; border-radius:999px; background:#cbd5e1; transition:background .15s; }
:global(html.dark .wifi-page button.radio-switch) { background:rgb(255 255 255 / .2); }
button.radio-switch[aria-checked=true] { background:#22c55e; }
:global(html.dark .wifi-page button.radio-switch[aria-checked=true]) { background:#22c55e; }
.switch-thumb { position:absolute; top:2px; left:2px; width:24px; height:24px; border-radius:50%; background:white; box-shadow:0 1px 3px rgb(0 0 0 / .15); transition:transform .15s; }
.radio-switch[aria-checked=true] .switch-thumb { transform:translateX(28px); }
button:focus-visible, input:focus-visible, select:focus-visible { outline:2px solid #38bdf8; outline-offset:2px; }
.band-state { margin-top:10px; font-size:12px; color:var(--wifi-muted); }
.client-header { display:flex; align-items:center; justify-content:space-between; gap:12px; margin-bottom:10px; }
.client-header h4 { min-width:0; overflow-wrap:anywhere; }
.client-band { flex-shrink:0; padding:3px 9px; border-radius:999px; background:rgb(148 163 184 / .12); color:var(--wifi-muted); font-size:12px; font-weight:500; }
.client-count { margin-left:6px; color:var(--wifi-muted); font-size:13px; font-weight:400; }
h4 { font-size:13px; font-weight:600; } h4 span { margin-left:5px; color:var(--wifi-muted); font-weight:400; }
.client { padding:12px 0; border-bottom:1px solid rgb(148 163 184 / .15); }
.client:last-child { border:0; padding-bottom:0; }
dl { display:grid; gap:6px; font-size:13px; } dl > div { display:grid; grid-template-columns:60px minmax(0,1fr); gap:10px; }
dt { color:var(--wifi-muted); } dd { min-width:0; overflow-wrap:anywhere; user-select:text; } dd span { display:block; }
.address { font-family:ui-monospace,SFMono-Regular,Consolas,monospace; font-size:12px; }
.empty { margin-top:8px; font-size:13px; color:var(--wifi-muted); }
.client-warning { color:#b45309; font-size:12px; overflow-wrap:anywhere; margin-top:8px; }
:global(html.dark .wifi-page .client-warning) { color:#fbbf24; }
.config-form { border-top:1px solid rgb(148 163 184 / .2); margin-top:14px; padding-top:14px; display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:12px; }
.config-form label { display:grid; gap:5px; font-size:13px; min-width:0; }
.config-form input:not([type=checkbox]), select { width:100%; min-width:0; border:1px solid rgb(148 163 184 / .4); border-radius:8px; padding:8px 10px; background:transparent; }
:global(html.dark .wifi-page option) { background:#1e293b; }
.config-form .hidden-control { display:flex; align-items:center; gap:8px; }
.form-actions { grid-column:1/-1; justify-content:flex-end; }
@media (max-width:1100px) { .bands { grid-template-columns:minmax(0,1fr); } }
@media (max-width:440px) { .wifi-card { padding:14px; } .config-form { grid-template-columns:minmax(0,1fr); } }
@media (prefers-reduced-motion:reduce) { button, .switch-thumb { transition:none; } }
</style>
