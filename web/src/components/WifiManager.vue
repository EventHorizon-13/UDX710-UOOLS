<script setup>
import { ref, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { authFetch } from '../composables/useApi'
import { useToast } from '../composables/useToast'

const { t } = useI18n()
const { success, error: showError } = useToast()
const bands = ref([])
const loading = ref(true)
const busyBand = ref('')
const supported = ref(true)

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
  <section class="space-y-6">
    <header class="flex flex-wrap items-end justify-between gap-4">
      <div>
        <p class="mb-2 text-xs font-semibold uppercase tracking-[0.22em] text-sky-500">CW52 · HOSTAPD</p>
        <h2 class="text-2xl font-bold text-slate-900 dark:text-white">{{ t('wifi.title') }}</h2>
        <p class="mt-1 text-sm text-slate-500 dark:text-white/55">{{ t('wifi.subtitle') }}</p>
      </div>
      <button @click="loadStatus" :disabled="loading" class="rounded-xl border border-slate-200 bg-white px-4 py-2 text-sm font-semibold text-slate-700 shadow-sm transition hover:-translate-y-0.5 hover:shadow-md disabled:opacity-50 dark:border-white/10 dark:bg-white/5 dark:text-white/80">
        <font-awesome-icon icon="sync-alt" class="mr-2" :class="{ 'animate-spin': loading }" />{{ t('wifi.refresh') }}
      </button>
    </header>

    <div v-if="loading && !bands.length" class="rounded-2xl border border-slate-200 bg-white/70 p-10 text-center text-slate-500 dark:border-white/10 dark:bg-white/5 dark:text-white/50">
      <span class="inline-block h-7 w-7 animate-spin rounded-full border-2 border-sky-500/30 border-t-sky-500"></span>
    </div>
    <div v-else-if="!supported" class="rounded-2xl border border-amber-300/60 bg-amber-50 p-6 text-amber-800 dark:border-amber-300/20 dark:bg-amber-500/10 dark:text-amber-200">{{ t('wifi.unsupported') }}</div>
    <div v-else class="grid gap-4 xl:grid-cols-2">
      <article v-for="band in bands" :key="band.interface" class="overflow-hidden rounded-2xl border border-slate-200/80 bg-white/80 shadow-sm dark:border-white/10 dark:bg-slate-900/55">
        <div class="flex items-start justify-between gap-4 border-b border-slate-200/70 p-5 dark:border-white/10">
          <div class="min-w-0">
            <p class="text-xs font-medium text-slate-500 dark:text-white/45">{{ band.interface }}</p>
            <h3 class="mt-1 truncate text-lg font-bold text-slate-900 dark:text-white">{{ band.interface === 'wlan1' ? t('wifi.band24') : t('wifi.band5') }}</h3>
            <p class="mt-2 truncate text-sm text-slate-600 dark:text-white/65">{{ band.ssid || '—' }}</p>
          </div>
          <span class="shrink-0 rounded-full px-3 py-1 text-xs font-semibold" :class="band.enabled ? 'bg-emerald-100 text-emerald-700 dark:bg-emerald-400/15 dark:text-emerald-300' : 'bg-slate-100 text-slate-500 dark:bg-white/10 dark:text-white/50'">
            {{ band.enabled ? t('wifi.enabled') : t('wifi.disabled') }}
          </span>
        </div>
        <div class="grid grid-cols-2 gap-3 p-5">
          <div class="rounded-xl bg-slate-100/80 p-3 dark:bg-white/5">
            <p class="text-xs text-slate-500 dark:text-white/45">{{ t('wifi.channel') }}</p>
            <p class="mt-1 font-semibold text-slate-800 dark:text-white/85">{{ band.channel || '—' }}</p>
          </div>
          <div class="rounded-xl bg-slate-100/80 p-3 dark:bg-white/5">
            <p class="text-xs text-slate-500 dark:text-white/45">{{ t('wifi.clients') }}</p>
            <p class="mt-1 font-semibold text-slate-800 dark:text-white/85">{{ band.clients?.length || 0 }}</p>
          </div>
        </div>
        <div class="px-5 pb-5">
          <button @click="setRadio(band)" :disabled="busyBand === band.interface || !band.available" class="w-full rounded-xl px-4 py-3 text-sm font-semibold text-white shadow transition hover:-translate-y-0.5 disabled:cursor-wait disabled:opacity-50" :class="band.enabled ? 'bg-slate-700 hover:bg-slate-800 dark:bg-slate-700 dark:hover:bg-slate-600' : 'bg-gradient-to-r from-sky-500 to-blue-500 hover:from-sky-400 hover:to-blue-400'">
            {{ busyBand === band.interface ? t('common.loading') : (band.enabled ? t('wifi.disable') : t('wifi.enable')) }}
          </button>
        </div>
        <div class="border-t border-slate-200/70 px-5 py-4 dark:border-white/10">
          <p class="mb-3 text-xs font-semibold uppercase tracking-wider text-slate-500 dark:text-white/45">{{ t('wifi.clients') }}</p>
          <div v-if="band.clients?.length" class="flex flex-wrap gap-2">
            <code v-for="client in band.clients" :key="client" class="rounded-lg bg-slate-100 px-2.5 py-1.5 text-xs text-slate-700 dark:bg-white/5 dark:text-white/70">{{ client }}</code>
          </div>
          <p v-else class="text-sm text-slate-400 dark:text-white/35">{{ t('wifi.noClients') }}</p>
        </div>
      </article>
    </div>
  </section>
</template>
