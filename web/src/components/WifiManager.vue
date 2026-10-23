<script setup>
import { ref, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { authFetch } from '../composables/useApi'
import { useToast } from '../composables/useToast'
import LanConfig from './LanConfig.vue'

const { t } = useI18n()
const { success, error: showError } = useToast()
const bands = ref([])
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
            <button @click="setRadio(band)" :disabled="busyBand === band.interface" :aria-label="band.enabled ? t('wifi.disable') : t('wifi.enable')" :aria-pressed="band.enabled" :class="{ enabled: band.enabled }">{{ band.enabled ? t('wifi.enabled') : t('wifi.disabled') }}</button>
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
        <div class="clients">
          <h4>{{ t('wifi.clients') }} <span>{{ band.clients_complete === false ? '—' : band.clients?.length || 0 }}</span></h4>
          <p v-if="band.client_warnings?.length" class="client-warning" role="status">{{ t('wifi.clientWarning') }}: {{ band.client_warnings.join('; ') }}</p>
          <div v-for="client in band.clients" :key="client.mac" class="client">
            <dl>
              <div><dt>{{ t('wifi.hostname') }}</dt><dd>{{ client.hostname || '—' }}</dd></div>
              <div><dt>MAC</dt><dd class="address">{{ client.mac }}</dd></div>
              <div><dt>IPv4</dt><dd class="address"><span v-for="ip in client.ipv4" :key="ip">{{ ip }}</span><span v-if="!client.ipv4?.length">—</span></dd></div>
              <div><dt>IPv6</dt><dd class="address"><span v-for="ip in client.ipv6" :key="ip">{{ ip }}</span><span v-if="!client.ipv6?.length">—</span></dd></div>
            </dl>
          </div>
          <p v-if="!band.clients?.length && band.clients_complete !== false" class="empty">{{ t('wifi.noClients') }}</p>
        </div>
      </article>
    </div>
    <LanConfig />
  </section>
</template>

<style scoped>
.wifi-page { display:grid; gap:16px; min-width:0; }
.toolbar { display:flex; justify-content:flex-end; }
.bands { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:16px; align-items:start; }
.wifi-card { min-width:0; border:1px solid rgb(148 163 184 / .25); border-radius:16px; padding:16px; background:rgb(255 255 255 / .65); }
:global(.dark) .wifi-card { background:rgb(15 23 42 / .4); }
.band-header { display:flex; flex-wrap:wrap; align-items:start; justify-content:space-between; gap:12px; }
.band-name { min-width:0; flex:1 1 100px; overflow-wrap:anywhere; }
h3 { font-size:16px; font-weight:700; } .band-name p { font-size:14px; margin-top:3px; }
.actions, .form-actions { display:flex; flex-wrap:wrap; gap:8px; }
button { border:1px solid rgb(148 163 184 / .4); border-radius:8px; padding:7px 10px; font-size:13px; transition:background .15s; }
button:hover:enabled { background:rgb(148 163 184 / .12); } button:disabled { opacity:.45; cursor:default; }
button.enabled { color:#059669; background:rgb(16 185 129 / .1); border-color:rgb(16 185 129 / .3); }
:global(.dark) button.enabled { color:#6ee7b7; }
button:focus-visible, input:focus-visible, select:focus-visible { outline:2px solid #38bdf8; outline-offset:2px; }
.band-state { margin-top:10px; font-size:12px; opacity:.65; }
.clients { margin-top:14px; border-top:1px solid rgb(148 163 184 / .2); padding-top:12px; }
h4 { font-size:13px; font-weight:600; } h4 span { margin-left:5px; opacity:.6; font-weight:400; }
.client { padding:12px 0; border-bottom:1px solid rgb(148 163 184 / .15); }
.client:last-child { border:0; padding-bottom:0; }
dl { display:grid; gap:6px; font-size:13px; } dl > div { display:grid; grid-template-columns:60px minmax(0,1fr); gap:10px; }
dt { opacity:.6; } dd { min-width:0; overflow-wrap:anywhere; user-select:text; } dd span { display:block; }
.address { font-family:ui-monospace,SFMono-Regular,Consolas,monospace; font-size:12px; }
.empty { margin-top:8px; font-size:13px; opacity:.55; }
.client-warning { color:#b45309; font-size:12px; overflow-wrap:anywhere; margin-top:8px; }
:global(.dark) .client-warning { color:#fbbf24; }
.config-form { border-top:1px solid rgb(148 163 184 / .2); margin-top:14px; padding-top:14px; display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:12px; }
.config-form label { display:grid; gap:5px; font-size:13px; min-width:0; }
.config-form input:not([type=checkbox]), select { width:100%; min-width:0; border:1px solid rgb(148 163 184 / .4); border-radius:8px; padding:8px 10px; background:transparent; }
:global(.dark) option { background:#1e293b; }
.config-form .hidden-control { display:flex; align-items:center; gap:8px; }
.form-actions { grid-column:1/-1; justify-content:flex-end; }
@media (max-width:1100px) { .bands { grid-template-columns:minmax(0,1fr); } }
@media (max-width:440px) { .wifi-card { padding:14px; } .config-form { grid-template-columns:minmax(0,1fr); } }
@media (prefers-reduced-motion:reduce) { button { transition:none; } }
</style>
