<script setup>
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { useI18n } from 'vue-i18n'

const { t } = useI18n()
const iframeRef = ref(null)
const isFullscreen = ref(false)
const loading = ref(true)
const terminalReady = ref(false)
const terminalError = ref('')
const isMobile = ref(false)

// 检测移动端
function checkMobile() {
  isMobile.value = window.innerWidth < 768
}

onMounted(() => {
  checkMobile()
  window.addEventListener('resize', checkMobile)
})

onUnmounted(() => {
  window.removeEventListener('resize', checkMobile)
})

const terminalUrl = computed(() => new URL('/terminal/', window.location.origin).href)

async function refreshTerminal() {
  loading.value = true
  terminalReady.value = false
  terminalError.value = ''
  try {
    const response = await fetch('/api/terminal/session', {
      method: 'POST',
      headers: { Authorization: `Bearer ${localStorage.getItem('auth_token') || ''}` }
    })
    if (!response.ok) throw new Error(t('terminal.loading') + ' (' + response.status + ')' )
    terminalReady.value = true
  } catch (error) {
    terminalError.value = error.message
    loading.value = false
  }
}

onMounted(refreshTerminal)

function toggleFullscreen() {
  isFullscreen.value = !isFullscreen.value
}

function onIframeLoad() {
  loading.value = false
}

function openInNewTab() {
  window.open(terminalUrl.value, '_blank')
}
</script>

<template>
  <div class="space-y-4">
    <!-- 终端容器 -->
    <div 
      class="rounded-2xl bg-white/95 dark:bg-white/5 backdrop-blur border border-slate-200/60 dark:border-white/10 shadow-lg shadow-slate-200/40 dark:shadow-black/20 hover:shadow-xl hover:shadow-slate-300/50 dark:hover:shadow-black/30 transition-all duration-300 overflow-hidden"
      :class="isFullscreen ? 'fixed inset-4 z-50 rounded-2xl' : ''"
    >
      <!-- 标题栏 -->
      <div class="flex items-center justify-between px-3 md:px-4 py-2 md:py-3 border-b border-slate-200/60 dark:border-white/10 bg-slate-50/80 dark:bg-black/20">
        <div class="flex items-center space-x-2 md:space-x-3">
          <div class="flex space-x-1 md:space-x-1.5">
            <span class="w-2.5 h-2.5 md:w-3 md:h-3 rounded-full bg-red-500"></span>
            <span class="w-2.5 h-2.5 md:w-3 md:h-3 rounded-full bg-yellow-500"></span>
            <span class="w-2.5 h-2.5 md:w-3 md:h-3 rounded-full bg-green-500"></span>
          </div>
          <h3 class="text-slate-900 dark:text-white font-semibold flex items-center text-sm md:text-base">
            <i class="fas fa-terminal text-slate-600 dark:text-slate-400 mr-1.5 md:mr-2"></i>
            {{ $t('terminal.title') }}
          </h3>
          
        </div>
        <div class="flex items-center space-x-1.5 md:space-x-2">
          <!-- 加载指示器 -->
          <span v-if="loading" class="text-xs text-slate-500 dark:text-white/50 items-center hidden sm:flex">
            <i class="fas fa-spinner animate-spin mr-1"></i>{{ $t('terminal.loading') }}
          </span>
          <!-- 新窗口打开 -->
          <button 
            @click="openInNewTab"
            class="p-1.5 md:p-2 rounded-lg bg-slate-200 dark:bg-white/10 hover:bg-slate-300 dark:hover:bg-white/20 transition-colors"
            :title="$t('terminal.openNewTab')"
          >
            <i class="fas fa-external-link-alt text-slate-600 dark:text-white/60 text-xs md:text-sm"></i>
          </button>
          <!-- 刷新按钮 -->
          <button 
            @click="refreshTerminal"
            class="p-1.5 md:p-2 rounded-lg bg-slate-200 dark:bg-white/10 hover:bg-slate-300 dark:hover:bg-white/20 transition-colors"
            :title="$t('terminal.refresh')"
          >
            <i class="fas fa-sync-alt text-slate-600 dark:text-white/60 text-xs md:text-sm" :class="{ 'animate-spin': loading }"></i>
          </button>
          <!-- 全屏按钮 -->
          <button 
            @click="toggleFullscreen"
            class="p-1.5 md:p-2 rounded-lg bg-slate-200 dark:bg-white/10 hover:bg-slate-300 dark:hover:bg-white/20 transition-colors"
            :title="isFullscreen ? $t('terminal.exitFullscreen') : $t('terminal.fullscreen')"
          >
            <i :class="isFullscreen ? 'fas fa-compress' : 'fas fa-expand'" class="text-slate-600 dark:text-white/60 text-xs md:text-sm"></i>
          </button>
        </div>
      </div>
      
      <!-- 终端iframe -->
      <div 
        class="bg-slate-900 dark:bg-black" 
        :class="isFullscreen ? 'h-[calc(100%-52px)]' : (isMobile ? 'h-[400px]' : 'h-[600px]')"
      >
        <p v-if="terminalError" class="p-4 text-red-400">{{ terminalError }}</p>
        <iframe
          v-if="terminalReady"
          ref="iframeRef"
          :src="terminalUrl"
          @load="onIframeLoad"
          class="w-full h-full border-0"
          allow="clipboard-read; clipboard-write"
        ></iframe>
      </div>
    </div>

    <!-- 全屏遮罩 -->
    <div 
      v-if="isFullscreen"
      class="fixed inset-0 bg-black/60 z-40"
      @click="toggleFullscreen"
    ></div>

  </div>
</template>

<style scoped>
iframe {
  background: #1e293b;
}
</style>

