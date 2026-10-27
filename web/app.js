// MORFO — Frontend application
// Copyright (C) 2026 José Pedro Trindade
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

'use strict';
const $ = id => document.getElementById(id);
const h = v => String(v ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const number = new Intl.NumberFormat('pt-BR');
let images = [], selectedFiles = [], rootMetricRecords = [], current = null, mode = 'original', currentUser = null, currentAnalysis = null, analysisRuns = [], selectedRegion = null;
let viewer = {imageId:null, manifest:null, scale:1, x:0, y:0, fit:true, drag:null, revision:0, frame:0};
const viewNames = {acervo:'Acervo de imagens',recebimento:'Recebimento',analises:'Análises radiculares',integridade:'Integridade e origem',sobre:'Sobre o MORFO',usuarios:'Gestão de Usuários & Acesso'};

const STRATEGY_DESCRIPTIONS = {
  BAYESIAN_CALIBRATED: {
    name: 'Nível 1: Calibração Bayesiana (Recomendado)',
    badge: 'Inferência Probabilística',
    summary: 'Modela a distribuição estatística de cor, luminância e textura das amostras com atualização contínua de verossimilhança. Rápido, determinístico e excelente para contraste padrão solo-raiz.'
  },
  TABULAR_INCREMENTAL: {
    name: 'Nível 2: Classificador Tabular (Online ML)',
    badge: 'Aprendizado de Máquina Incremental',
    summary: 'Extrai 6 atributos regionais (razões local, global e compartilhada, área normalizada, aspecto e contato com borda) e treina classificador linear online a cada revisão humana.'
  },
  VISUAL_KNN: {
    name: 'Nível 3: Similaridade Visual k-NN & Active Learning',
    badge: 'Aprendizado Ativo Guiado',
    summary: 'Mapeia vetores de descritores no espaço métrico multidimensional. Prioriza automaticamente amostras com maior incerteza para acelerar a calibração com menos esforço.'
  },
  BASELINE_HEURISTIC: {
    name: 'Baseline Heurístico (Estático)',
    badge: 'Regras Tradicionais',
    summary: 'Segmentação determinística por limiares morfológicos fixos sem aprendizado incremental.'
  }
};

function getStrategyDescHtml(key){
  const s = STRATEGY_DESCRIPTIONS[key] || STRATEGY_DESCRIPTIONS.BAYESIAN_CALIBRATED;
  return `<strong>${h(s.badge)}</strong><span>${h(s.summary)}</span>`;
}

function toast(message){
  $('toast').textContent = message;
  $('toast').classList.add('visible');
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => $('toast').classList.remove('visible'), 4400);
}

function nav(view){
  for(const name of Object.keys(viewNames)){
    $('view-'+name).classList.toggle('hidden', view !== name);
  }
  for(const b of document.querySelectorAll('[data-view]')){
    let active = b.dataset.view === view;
    b.classList.toggle('active', active);
    if(active) b.setAttribute('aria-current', 'page');
    else b.removeAttribute('aria-current');
  }
  $('page-name').textContent = viewNames[view];
  $('sidebar').classList.remove('open');
  $('mobile-scrim').classList.remove('shown');
  $('menu').setAttribute('aria-expanded', 'false');
  location.hash = view;
  window.scrollTo(0, 0);
  if(view==='analises'){showRootAnalysisCatalog();loadRootMetricRecords();}
  if(view==='usuarios'){loadUsers();}
 }
async function api(path, options = {}){
  const r = await fetch(path, options);
  let data = await r.json().catch(() => ({error: 'Resposta inválida do servidor'}));
  if(!r.ok){
    if(r.status === 401 && !path.startsWith('/api/v1/auth/')){
      currentUser = null;
      updateUserUI();
      showAuthOverlay('Sua sessão expirou ou é necessária autenticação.');
    }
    const err = new Error(data.error || `Erro HTTP ${r.status}`);
    err.status = r.status;
    err.data = data;
    throw err;
  }
  return data;
}

function getInitials(name, email){
  if(name && name !== email){
    const parts = name.trim().split(/\s+/);
    if(parts.length >= 2) return (parts[0][0] + parts[parts.length - 1][0]).toUpperCase();
    if(parts.length === 1 && parts[0].length >= 2) return parts[0].slice(0, 2).toUpperCase();
  }
  const clean = (email || '').split('@')[0].replace(/[^a-zA-Z]/g, '');
  return clean.slice(0, 2).toUpperCase() || 'EM';
}

function updateUserUI(){
  const chip = $('user-chip');
  const opEmail = $('operator-email');
  const navUsuarios = $('nav-btn-usuarios');
  if(currentUser){
    chip.classList.remove('hidden');
    $('user-avatar').textContent = getInitials(currentUser.name, currentUser.email);
    $('user-email').textContent = currentUser.email;
    $('user-email').title = currentUser.name ? `${currentUser.name} (${currentUser.email})` : currentUser.email;
    const roleLabel = currentUser.role === 'admin' ? 'Administrador' : currentUser.role === 'researcher' ? 'Pesquisador' : 'Usuário';
    const roleEl = document.querySelector('.user-role');
    if(roleEl) roleEl.textContent = roleLabel;
    if(opEmail) opEmail.textContent = currentUser.email;
    if(navUsuarios){
      navUsuarios.classList.toggle('hidden', currentUser.role !== 'admin');
    }
  } else {
    chip.classList.add('hidden');
    if(opEmail) opEmail.textContent = 'Não autenticado';
    if(navUsuarios) navUsuarios.classList.add('hidden');
  }
}

function setAuthFeedback(msg, type = 'error'){
  const box = $('auth-feedback');
  if(!msg){ box.classList.add('hidden'); box.textContent = ''; return; }
  box.className = `auth-feedback ${type}`;
  box.textContent = msg;
  box.classList.remove('hidden');
}

function showAuthOverlay(infoMsg){
  $('auth-overlay').classList.remove('hidden');
  document.body.style.overflow = 'hidden';
  $('auth-email-form').classList.remove('hidden');
  $('auth-code-form').classList.add('hidden');
  $('auth-dev-preview').classList.add('hidden');
  setAuthFeedback(infoMsg || '', infoMsg ? 'error' : '');
  setTimeout(() => $('auth-email-input').focus(), 50);
}

function hideAuthOverlay(){
  $('auth-overlay').classList.add('hidden');
  document.body.style.overflow = '';
}

let cooldownTimer = null;
function startCooldownTimer(seconds = 60){
  if(cooldownTimer) clearInterval(cooldownTimer);
  let remaining = seconds;
  const resendBtn = $('btn-resend-code');
  if(!resendBtn) return;
  const originalText = '↻ Reenviar novo código';
  resendBtn.disabled = true;
  resendBtn.textContent = `Aguarde ${remaining}s para reenviar`;

  cooldownTimer = setInterval(() => {
    remaining -= 1;
    if(remaining <= 0){
      clearInterval(cooldownTimer);
      cooldownTimer = null;
      resendBtn.disabled = false;
      resendBtn.textContent = originalText;
    } else {
      resendBtn.textContent = `Aguarde ${remaining}s para reenviar`;
    }
  }, 1000);
}

async function requestAuthCode(e){
  if(e && e.preventDefault) e.preventDefault();
  const emailInput = $('auth-email-input');
  const email = emailInput.value.trim().toLowerCase();
  if(!email || !email.includes('@') || !email.includes('.')){
    setAuthFeedback('Por favor, informe um endereço de e-mail válido.', 'error');
    emailInput.focus();
    return;
  }
  const btn = $('btn-request-code');
  const resendBtn = $('btn-resend-code');
  btn.disabled = true;
  if(resendBtn) resendBtn.disabled = true;
  setAuthFeedback('Enviando código de verificação por e-mail...', 'success');
  try{
    const res = await api('/api/v1/auth/request-code', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({email})
    });
    $('auth-target-email').textContent = email;
    $('auth-email-form').classList.add('hidden');
    $('auth-code-form').classList.remove('hidden');
    if(res.dev_code){
      $('auth-dev-preview').classList.remove('hidden');
      $('auth-dev-code-val').textContent = res.dev_code;
    } else {
      $('auth-dev-preview').classList.add('hidden');
    }
    setAuthFeedback(res.message || 'Código numérico enviado! Verifique seu e-mail.', 'success');
    $('auth-code-input').value = '';
    setTimeout(() => $('auth-code-input').focus(), 50);
    startCooldownTimer(60);
  }catch(err){
    if(err.status === 429 && err.data && err.data.retry_after_seconds){
      startCooldownTimer(err.data.retry_after_seconds);
    }
    setAuthFeedback(err.message, 'error');
  }finally{
    btn.disabled = false;
  }
}

async function verifyAuthCode(e){
  if(e && e.preventDefault) e.preventDefault();
  const email = $('auth-email-input').value.trim().toLowerCase();
  const code = $('auth-code-input').value.trim();
  if(!code || code.length !== 6){
    setAuthFeedback('Por favor, informe o código numérico de 6 dígitos.', 'error');
    $('auth-code-input').focus();
    return;
  }
  const btn = $('btn-verify-code');
  btn.disabled = true;
  setAuthFeedback('Validando código de verificação...', 'success');
  try{
    const res = await api('/api/v1/auth/verify-code', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({email, code})
    });
    currentUser = res.user;
    updateUserUI();
    hideAuthOverlay();
    toast(`Bem-vindo(a), ${currentUser.name || currentUser.email}!`);
    await refresh();
  }catch(err){
    setAuthFeedback(err.message, 'error');
    if(err.status === 403){
      $('auth-code-input').value = '';
    }
  }finally{
    btn.disabled = false;
  }
}

async function logout(){
  try{
    await api('/api/v1/auth/logout', {method: 'POST'});
  }catch(_){}
  currentUser = null;
  updateUserUI();
  images = [];
  rootMetricRecords = [];
  render();
  showAuthOverlay('Sessão encerrada com segurança.');
  toast('Sessão encerrada.');
}

function render(){
  const search = $('search').value.trim().toLowerCase(), filter = $('filter').value;
  let items = images.filter(x => (filter === 'all' || filter === x.qc_state) && (!search || [x.filename, x.sample_id, x.sha256, x.id, x.user_email].join(' ').toLowerCase().includes(search)));
  $('stat-total').textContent = number.format(images.length);
  $('stat-prepared').textContent = number.format(images.length);
  $('stat-review').textContent = number.format(images.filter(i => i.qc_state !== 'REVIEW').length);
  if(!items.length){
    $('gallery').innerHTML = `<div class="empty"><svg viewBox="0 0 52 52" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="5" y="8" width="42" height="36" rx="5"/><path d="m10 37 12-13 8 7 6-10 10 16"/><circle cx="16" cy="18" r="3"/></svg><h3>${images.length ? 'Nenhuma imagem corresponde aos filtros' : 'Seu acervo começa com uma imagem'}</h3><p>${images.length ? 'Experimente outra pesquisa ou limpe os filtros.' : 'Envie os primeiros registros para preservar originais e gerar prévias para inspeção.'}</p>${images.length ? '' : '<button class="button primary" id="empty-upload">Receber primeira imagem →</button>'}</div>`;
    if($('empty-upload')) $('empty-upload').onclick = () => nav('recebimento');
    return;
  }
  $('gallery').innerHTML = items.map(x => `<article class="image-card"><button class="card-open" type="button" data-image="${h(x.id)}" aria-label="Inspecionar ${h(x.filename)}"><div class="thumb"><img src="/api/v1/images/${h(x.id)}/preview" alt="" loading="lazy"><span class="thumb-tag">${number.format(x.width)} × ${number.format(x.height)}</span></div><div class="cardbody"><strong title="${h(x.filename)}">${h(x.filename)}</strong><p>${h(x.sample_id || 'Sem amostra')} · ${h(x.capture_date || x.created_at.slice(0, 10))}</p><div class="card-foot"><span class="quality ${x.qc_state === 'REVIEW' ? '' : 'alert'}">${x.qc_state === 'REVIEW' ? 'Inspeção visual' : x.qc_state === 'LOW_LIGHT' ? 'Pouca luz' : 'Excesso de luz'}</span><span class="open-link">Inspecionar ↗</span></div></div></button><button class="card-refine" type="button" data-refine-image="${h(x.id)}">◫ Refinar classificação</button></article>`).join('');
  for(const item of document.querySelectorAll('[data-image]')) item.onclick = () => openInspect(item.dataset.image);
  for(const item of document.querySelectorAll('[data-refine-image]')) item.onclick = () => {openInspect(item.dataset.refineImage);openRefinement();};
}

function metricValue(value,unit,fallback='—'){
  return value==null?fallback:`${Number(value).toLocaleString('pt-BR',{maximumFractionDigits:4})}${unit?` ${unit}`:''}`;
}
function showRootAnalysisCatalog(){
  document.querySelector('.root-analysis-catalog')?.classList.remove('hidden');
  $('root-analysis-detail')?.classList.add('hidden');
}
function rootMetricCell(label,value){return `<div><dt>${h(label)}</dt><dd>${value}</dd></div>`;}
function rootMetricStatusLabel(status){return ({NOT_VALIDATED:'Não validado',REVIEWED:'Revisado',ARCHIVED:'Arquivado'})[status]||status;}
async function openRootMetricDetail(metricId){
  const detail=$('root-analysis-detail'),catalog=document.querySelector('.root-analysis-catalog');
  if(!detail)return;
  catalog?.classList.add('hidden');detail.classList.remove('hidden');detail.innerHTML='<div class="empty"><h3>Carregando registro…</h3></div>';window.scrollTo(0,0);
  try{
    const response=await api(`/api/v1/root-metrics/${encodeURIComponent(metricId)}`),metric=response.data||response;
    const record=rootMetricRecords.find(item=>item.id===metricId)||{};
    const unit=metric.diameter_unit||'px',components=Array.isArray(metric.components)?metric.components:[],classes=Array.isArray(metric.diameter_classes)?metric.diameter_classes:[];
    const roiTotal=Number(metric.roi_pixels)||0,coverageParts=[{label:'Candidatos',value:Number(metric.candidate_pixels)||0,className:'candidate'},{label:'Matriz',value:Number(metric.matrix_pixels)||0,className:'matrix'},{label:'Excluídos',value:Number(metric.excluded_or_abstained_pixels)||0,className:'excluded'}];
    const topologyParts=[{label:'Pontas',value:metric.apparent_tips},{label:'Ramificações',value:metric.apparent_branches},{label:'Truncadas',value:metric.truncated_tips}],topologyMax=Math.max(1,...topologyParts.map(item=>Number(item.value)||0));
    const componentChart=components.slice().sort((a,b)=>(Number(b.projected_length_mm??b.projected_length_px)||0)-(Number(a.projected_length_mm??a.projected_length_px)||0)).slice(0,12),componentMax=Math.max(1,...componentChart.map(item=>Number(item.projected_length_mm??item.projected_length_px)||0));
    const isRefinement=Boolean(record.refinement_id||metric.refinement_id);
    const refinementId=record.refinement_id||metric.refinement_id;
    const overlayPath=isRefinement?`/api/v1/refinements/${encodeURIComponent(refinementId)}/overlay`:`/api/v1/images/${encodeURIComponent(record.image_id||'')}/profiles/roots-exp003/${encodeURIComponent(record.profile_run_id||metric.profile_run_id||'')}/overlay`;
    const originalPath=`/api/v1/images/${encodeURIComponent(record.image_id||'')}/original-view`;
    const imageWidth=Number(metric.width??record.width);
    const imageHeight=Number(metric.height??record.height);
    const hasImageGeometry=Boolean(record.image_id)&&Number.isFinite(imageWidth)&&imageWidth>0&&Number.isFinite(imageHeight)&&imageHeight>0;
    const labelSize=hasImageGeometry?Math.max(10,Math.min(36,Math.round(Math.min(imageWidth,imageHeight)/40))):12;
    detail.innerHTML=`
      <div class="root-detail-head"><button class="button outline" id="root-detail-back" type="button">← Voltar às análises</button><div class="root-detail-actions"><a class="button outline" href="/api/v1/root-metrics/${encodeURIComponent(metricId)}/export.csv" download>↓ CSV</a><a class="button outline" href="/api/v1/root-metrics/${encodeURIComponent(metricId)}/export.json" download>↓ JSON</a></div></div>
      <header class="root-detail-title"><div><div class="analysis-card-kicker"><span class="section-kicker">${h(record.sample_id||'SEM AMOSTRA')}</span><span class="analysis-origin-badge ${isRefinement?'tag-refinement':'tag-exp003'}">${isRefinement?'Refinamento':'EXP-003 Original'}</span></div><h2>${h(record.filename||'Análise radicular')}</h2><p>Registro ${h(metric.id||metricId)} · ${h(metric.created_at||record.created_at||'')}${isRefinement?` · Sessão de Refinamento: ${h(refinementId)}`:''}</p></div><span class="metric-status ${h((metric.record_status||record.status||'').toLowerCase())}">${h(rootMetricStatusLabel(metric.record_status||record.status||''))}</span></header>
      <section class="root-detail-section"><h3>Resumo morfométrico</h3><dl class="root-detail-metrics">
        ${rootMetricCell('Área candidata',metric.projected_area_mm2==null?metricValue(metric.candidate_pixels,'px²'):metricValue(metric.projected_area_mm2,'mm²'))}
        ${rootMetricCell('Comprimento',metric.projected_length_mm==null?metricValue(metric.projected_length_px,'px'):metricValue(metric.projected_length_mm,'mm'))}
        ${rootMetricCell('Diâmetro médio',metricValue(metric.mean_diameter,unit))}
        ${rootMetricCell('Diâmetro mínimo',metricValue(metric.min_diameter,unit))}
        ${rootMetricCell('Diâmetro máximo',metricValue(metric.max_diameter,unit))}
        ${rootMetricCell('Ocupação',metricValue(metric.occupancy_percent,'%'))}
        ${rootMetricCell('Pontas aparentes',metricValue(metric.apparent_tips,''))}
        ${rootMetricCell('Pontas truncadas',metricValue(metric.truncated_tips,''))}
        ${rootMetricCell('Ramificações aparentes',metricValue(metric.apparent_branches,''))}
        ${rootMetricCell('Componentes',metricValue(components.length,''))}
        ${rootMetricCell('Pixels de esqueleto',metricValue(metric.skeleton_pixels,'px'))}
        ${rootMetricCell('Fração mensurável',metric.measurable_diameter_length_fraction==null?'—':metricValue(metric.measurable_diameter_length_fraction*100,'%'))}
      </dl>${metric.apparent_tips==null?`<p class="root-detail-version-note">Este registro foi criado por ${h(metric.metrics_version||record.metrics_version||'uma versão anterior')}, que não persistia as métricas topológicas. “—” significa não medido, não zero. Recalcule a morfometria para gerar esses valores.</p>`:''}</section>
      <section class="root-detail-section"><h3>Cobertura e grafo</h3><dl class="root-detail-metrics">
        ${rootMetricCell('Dimensões',metric.width==null?'—':`${number.format(metric.width)} × ${number.format(metric.height)} px`)}
        ${rootMetricCell('Pixels da ROI',metricValue(metric.roi_pixels,'px'))}
        ${rootMetricCell('Pixels válidos',metricValue(metric.valid_pixels,'px'))}
        ${rootMetricCell('Pixels candidatos',metricValue(metric.candidate_pixels,'px'))}
        ${rootMetricCell('Pixels de matriz',metricValue(metric.matrix_pixels,'px'))}
        ${rootMetricCell('Excluídos/abstenção',metricValue(metric.excluded_or_abstained_pixels,'px'))}
        ${rootMetricCell('Arestas do grafo',metricValue(metric.graph_edges,''))}
        ${rootMetricCell('Pixels isolados',metricValue(metric.isolated_skeleton_pixels,'px'))}
        ${rootMetricCell('Braços incidentes',Array.isArray(metric.branch_incident_arms)&&metric.branch_incident_arms.length?metric.branch_incident_arms.map(value=>number.format(value)).join(', '):'—')}
      </dl></section>
      ${hasImageGeometry?`<section class="root-detail-section"><div class="root-visual-heading"><div><h3>Mapa dos componentes candidatos</h3><p>Os números correspondem aos IDs da tabela. Não representam identificação biológica validada.</p></div><div class="root-visual-controls"><button type="button" class="button outline active" data-root-layer="overlay">Sobreposição</button><button type="button" class="button outline" data-root-layer="original">Original</button><label><input id="root-box-toggle" type="checkbox" checked> Caixas</label><label><input id="root-label-toggle" type="checkbox" checked> Rótulos</label></div></div><div id="root-labeled-image" class="root-labeled-image"><img id="root-analysis-image" src="${overlayPath}" alt="Imagem da análise com componentes candidatos rotulados"><svg viewBox="0 0 ${imageWidth} ${imageHeight}" role="img" aria-label="Rótulos e caixas dos componentes candidatos">${components.map(item=>{const box=item.bounding_box||{},cx=Number(item.centroid_x_px),cy=Number(item.centroid_y_px);if(!Number.isFinite(cx)||!Number.isFinite(cy))return '';const validBox=[box.min_x,box.min_y,box.max_x,box.max_y].every(value=>Number.isFinite(Number(value)));return `<g data-root-component="${h(item.id)}">${validBox?`<rect class="component-box" x="${Number(box.min_x)}" y="${Number(box.min_y)}" width="${Math.max(1,Number(box.max_x)-Number(box.min_x)+1)}" height="${Math.max(1,Number(box.max_y)-Number(box.min_y)+1)}"/>`:''}<circle class="component-label-bg" cx="${cx}" cy="${cy}" r="${labelSize*.78}"/><text class="component-label" x="${cx}" y="${cy}" font-size="${labelSize}">${h(item.id)}</text></g>`;}).join('')}</svg></div><div class="root-visual-legend"><span><i></i> Componente candidato</span><span>${number.format(components.length)} componentes rotulados</span></div></section>`:''}
      <section class="root-detail-section"><h3>Representação gráfica</h3><div class="root-detail-charts">
        <article class="root-chart coverage-chart"><h4>Composição da ROI</h4>${roiTotal?`<div class="coverage-stack" aria-label="Composição da ROI">${coverageParts.map(item=>`<i class="${item.className}" style="width:${Math.max(0,100*item.value/roiTotal)}%" title="${h(item.label)}: ${number.format(item.value)} px"></i>`).join('')}</div><div class="chart-legend">${coverageParts.map(item=>`<span><i class="${item.className}"></i><b>${h(item.label)}</b> ${metricValue(100*item.value/roiTotal,'%')}</span>`).join('')}</div>`:'<p class="root-detail-muted">ROI indisponível.</p>'}</article>
        <article class="root-chart topology-chart"><h4>Topologia aparente</h4>${metric.apparent_tips==null?'<p class="root-detail-muted">Não medida nesta versão.</p>':topologyParts.map(item=>`<div><span>${h(item.label)}</span><i style="--chart-value:${100*(Number(item.value)||0)/topologyMax}%"></i><b>${number.format(item.value)}</b></div>`).join('')}</article>
        <article class="root-chart component-chart"><h4>Maiores componentes · comprimento</h4>${componentChart.length?componentChart.map(item=>{const value=Number(item.projected_length_mm??item.projected_length_px)||0;return `<div><span>#${h(item.id)}</span><i style="--chart-value:${100*value/componentMax}%"></i><b>${metricValue(value,item.projected_length_mm==null?'px':'mm')}</b></div>`;}).join(''):'<p class="root-detail-muted">Sem componentes.</p>'}</article>
      </div></section>
      ${classes.length?`<section class="root-detail-section"><h3>Distribuição de diâmetros</h3><div class="root-detail-histogram">${classes.map(item=>`<div><span>${item.upper_bound==null?`≥ ${metricValue(item.lower_bound,unit)}`:`${metricValue(item.lower_bound,unit)} – ${metricValue(item.upper_bound,unit)}`}</span><i style="--diameter-percent:${Math.max(0,Math.min(100,Number(item.percent)||0))}%"></i><b>${metricValue(item.percent,'%')}</b></div>`).join('')}</div></section>`:''}
      <section class="root-detail-section"><h3>Componentes detectados</h3>${components.length?`<div class="root-detail-table"><table><thead><tr><th>ID</th><th>Área</th><th>Comprimento</th><th>Esqueleto</th><th>Arestas</th><th>Pontas</th><th>Ramificações</th><th>Centroide (x, y)</th><th>Caixa delimitadora (mín. → máx.)</th><th>Contato com limites</th></tr></thead><tbody>${components.map(item=>{const box=item.bounding_box||{};return `<tr><td>${h(item.id??item.component_id??'—')}</td><td>${item.area_mm2==null?metricValue(item.area_px,'px²'):metricValue(item.area_mm2,'mm²')}</td><td>${item.projected_length_mm==null?metricValue(item.projected_length_px,'px'):metricValue(item.projected_length_mm,'mm')}</td><td>${metricValue(item.skeleton_pixels,'px')}</td><td>${metricValue(item.graph_edges,'')}</td><td>${metricValue(item.apparent_tips,'')}</td><td>${metricValue(item.apparent_branches,'')}</td><td>${item.centroid_x_px==null?'—':`${metricValue(item.centroid_x_px,'')}, ${metricValue(item.centroid_y_px,'')}`}</td><td>${box.min_x==null?'—':`(${number.format(box.min_x)}, ${number.format(box.min_y)}) → (${number.format(box.max_x)}, ${number.format(box.max_y)})`}</td><td>${[item.touches_image_boundary&&'Imagem',item.touches_roi_boundary&&'ROI',item.touches_abstained_area&&'Abstenção'].filter(Boolean).join(', ')||'Nenhum'}</td></tr>`;}).join('')}</tbody></table></div>`:'<p class="root-detail-muted">Nenhum componente registrado.</p>'}</section>
      <div class="root-detail-columns"><section class="root-detail-section"><h3>Escala e proveniência</h3><dl class="root-detail-provenance"><dt>Origem</dt><dd>${isRefinement?`Refinamento Humano (Sessão: ${h(refinementId)})`:'Execução Original EXP-003'}</dd><dt>Escala X</dt><dd>${metricValue(metric.scale_x_mm_per_pixel,'mm/px')}</dd><dt>Escala Y</dt><dd>${metricValue(metric.scale_y_mm_per_pixel,'mm/px')}</dd><dt>Versão da métrica</dt><dd>${h(metric.metrics_version||record.metrics_version||'—')}</dd><dt>Execução do perfil</dt><dd>${h(metric.profile_run_id||record.profile_run_id||'—')}</dd><dt>Natureza</dt><dd>${h(metric.nature||'—')}</dd><dt>Estado científico</dt><dd>${h(metric.scientific_status||'—')}</dd></dl></section>
      <section class="root-detail-section"><h3>Curadoria do registro</h3><form id="root-detail-form" class="root-detail-form"><label>Estado<select id="root-detail-status"><option value="NOT_VALIDATED">Não validado</option><option value="REVIEWED">Revisado</option><option value="ARCHIVED">Arquivado</option></select></label><label>Notas<textarea id="root-detail-notes" maxlength="2000" rows="5" placeholder="Observações da revisão">${h(metric.notes||'')}</textarea></label><div><button class="button primary" type="submit">Salvar alterações</button><button class="button danger" id="root-detail-delete" type="button">Excluir registro</button></div></form></section></div>
      ${record.image_id?`<footer class="root-detail-source"><p>A imagem é mantida como fonte; este registro possui espaço próprio.</p><button class="button outline" id="root-detail-image" type="button">Abrir imagem de origem ↗</button></footer>`:''}`;
    $('root-detail-status').value=metric.record_status||record.status||'NOT_VALIDATED';
    $('root-detail-back').onclick=showRootAnalysisCatalog;
    document.querySelectorAll('[data-root-layer]').forEach(button=>button.onclick=()=>{document.querySelectorAll('[data-root-layer]').forEach(item=>item.classList.toggle('active',item===button));$('root-analysis-image').src=button.dataset.rootLayer==='original'?originalPath:overlayPath;});
    if($('root-box-toggle'))$('root-box-toggle').onchange=event=>$('root-labeled-image').classList.toggle('boxes-hidden',!event.target.checked);
    if($('root-label-toggle'))$('root-label-toggle').onchange=event=>$('root-labeled-image').classList.toggle('labels-hidden',!event.target.checked);
    $('root-detail-form').onsubmit=async event=>{event.preventDefault();try{const updated=await api(`/api/v1/root-metrics/${encodeURIComponent(metricId)}`,{method:'PATCH',headers:{'Content-Type':'application/json'},body:JSON.stringify({status:$('root-detail-status').value,notes:$('root-detail-notes').value})});const item=rootMetricRecords.find(entry=>entry.id===metricId);if(item){item.status=updated.data.record_status;item.notes=updated.data.notes;}toast('Registro atualizado.');openRootMetricDetail(metricId);}catch(error){toast(error.message);}};
    $('root-detail-delete').onclick=async()=>{if(!confirm('Excluir este registro de análise? A imagem de origem será preservada.'))return;try{await api(`/api/v1/root-metrics/${encodeURIComponent(metricId)}`,{method:'DELETE'});rootMetricRecords=rootMetricRecords.filter(item=>item.id!==metricId);renderRootMetricRecords();showRootAnalysisCatalog();toast('Registro de análise excluído.');}catch(error){toast(error.message);}};
    if($('root-detail-image'))$('root-detail-image').onclick=()=>openInspect(record.image_id);
  }catch(error){detail.innerHTML=`<button class="button outline" id="root-detail-back" type="button">← Voltar às análises</button><div class="empty"><h3>Falha ao carregar análise</h3><p>${h(error.message)}</p></div>`;$('root-detail-back').onclick=showRootAnalysisCatalog;}
}
function renderRootMetricRecords(){
  const target=$('root-analysis-list');if(!target)return;const search=$('root-analysis-search').value.trim().toLowerCase(),filter=$('root-analysis-filter').value;
  const records=rootMetricRecords.filter(record=>(filter==='all'||record.status===filter)&&(!search||[record.filename,record.sample_id,record.id,record.profile_run_id,record.status,record.refinement_id?'refinamento':'original exp-003'].join(' ').toLowerCase().includes(search)));
  if(!records.length){target.innerHTML=`<div class="empty"><h3>${rootMetricRecords.length?'Nenhuma análise corresponde aos filtros':'Nenhuma análise radicular registrada'}</h3><p>${rootMetricRecords.length?'Altere busca ou estado.':'Execute perfil EXP-003 em uma imagem; morfometria será registrada automaticamente.'}</p></div>`;return;}
  target.innerHTML=records.map(record=>{
    const isRefinement=Boolean(record.refinement_id);
    const originLabel=isRefinement?'Refinamento':'EXP-003 Original';
    const originClass=isRefinement?'tag-refinement':'tag-exp003';
    return `<article class="root-analysis-card ${isRefinement?'is-refinement':''}">
    <header>
      <div>
        <div class="analysis-card-kicker">
          <span class="section-kicker">${h(record.sample_id||'SEM AMOSTRA')}</span>
          <span class="analysis-origin-badge ${originClass}">${originLabel}</span>
        </div>
        <h3>${h(record.filename)}</h3>
        <small>${h(record.created_at)} · ID: ${h(record.id.slice(0,8))}${isRefinement?` · Refinamento: ${h(record.refinement_id.slice(0,8))}`:''}</small>
      </div>
      <span class="metric-status ${h(record.status.toLowerCase())}">${h(record.status)}</span>
    </header>
    <dl>
      <div><dt>Área candidata</dt><dd>${record.projected_area_mm2==null?number.format(record.candidate_pixels)+' px²':metricValue(record.projected_area_mm2,'mm²')}</dd></div>
      <div><dt>Comprimento</dt><dd>${record.projected_length_mm==null?metricValue(record.projected_length_px,'px'):metricValue(record.projected_length_mm,'mm')}</dd></div>
      <div><dt>Diâmetro médio</dt><dd>${metricValue(record.mean_diameter,record.diameter_unit)}</dd></div>
      <div><dt>Ocupação</dt><dd>${metricValue(record.occupancy_percent,'%')}</dd></div>
      <div><dt>Pontas</dt><dd>${metricValue(record.apparent_tips,'')}</dd></div>
      <div><dt>Ramificações</dt><dd>${metricValue(record.apparent_branches,'')}</dd></div>
    </dl>
    ${record.notes?`<p class="root-analysis-notes">${h(record.notes)}</p>`:''}
    <footer>
      <button type="button" class="button primary open-analysis-record" data-metric="${h(record.id)}">Ver análise →</button>
      <a class="button outline" href="/api/v1/root-metrics/${encodeURIComponent(record.id)}/export.csv" download>CSV</a>
      <a class="button outline" href="/api/v1/root-metrics/${encodeURIComponent(record.id)}/export.json" download>JSON</a>
      <button type="button" class="button danger delete-analysis-record" data-metric="${h(record.id)}" data-filename="${h(record.filename)}" data-origin="${originLabel}">Excluir análise</button>
    </footer>
  </article>`;
  }).join('');
  target.querySelectorAll('.open-analysis-record').forEach(button=>button.addEventListener('click',()=>openRootMetricDetail(button.dataset.metric)));
  target.querySelectorAll('.delete-analysis-record').forEach(button=>button.addEventListener('click',()=>deleteRootMetricRecord(button.dataset.metric,button.dataset.filename,button.dataset.origin)));
}
async function deleteRootMetricRecord(metricId,filename,origin=''){
 const label=origin?` (${origin})`:'';
 if(!confirm(`Excluir análise${label} de “${filename}”? A imagem original será preservada e a exclusão permanecerá auditável.`))return;
 try{await api(`/api/v1/root-metrics/${encodeURIComponent(metricId)}`,{method:'DELETE'});rootMetricRecords=rootMetricRecords.filter(item=>item.id!==metricId);renderRootMetricRecords();toast('Análise removida do catálogo. Imagem original preservada.');}catch(error){toast(error.message);}
}
async function loadRootMetricRecords(){
  const target=$('root-analysis-list');if(!target||!currentUser)return;if(!rootMetricRecords.length)target.innerHTML='<div class="empty"><h3>Carregando análises…</h3></div>';
  try{const response=await api('/api/v1/root-metrics');rootMetricRecords=Array.isArray(response.data)?response.data:[];renderRootMetricRecords();}catch(error){target.innerHTML=`<div class="empty"><h3>Falha ao carregar análises</h3><p>${h(error.message)}</p></div>`;}
}

async function refresh(){
  if(!currentUser){
    try{
      const auth = await api('/api/v1/auth/me');
      if(auth.authenticated){
        currentUser = auth.user;
        updateUserUI();
        hideAuthOverlay();
      } else {
        showAuthOverlay();
        return;
      }
    }catch(e){
      showAuthOverlay();
      return;
    }
  }
  try{
    const health = await api('/api/v1/health');
    if(health && health.version){
      const vEl = $('connection-version');
      if(vEl) vEl.textContent = 'v' + health.version;
      const ingEl = $('ingest-version');
      if(ingEl) ingEl.textContent = 'INGESTÃO v' + health.version;
    }
  }catch(_){}
  try{
    const res = await api('/api/v1/images');
    images = res.data || [];
    render();
    $('service-state').textContent = 'Serviço local ativo';
    $('service-state').parentElement.classList.remove('offline');
  }catch(e){
    $('service-state').textContent = 'Acesso restrito ou indisponível';
    $('service-state').parentElement.classList.add('offline');
    $('gallery').innerHTML = `<div class="empty"><h3>Não foi possível consultar o servidor</h3><p>${h(e.message)}.</p></div>`;
  }
}

function clampViewer(){
  if(!current) return;
  const stage = $('image-stage'), width = current.width * viewer.scale, height = current.height * viewer.scale;
  viewer.x = width <= stage.clientWidth ? (stage.clientWidth - width) / 2 : Math.min(0, Math.max(stage.clientWidth - width, viewer.x));
  viewer.y = height <= stage.clientHeight ? (stage.clientHeight - height) / 2 : Math.min(0, Math.max(stage.clientHeight - height, viewer.y));
}

function scheduleTiles(){
  cancelAnimationFrame(viewer.frame);
  viewer.frame = requestAnimationFrame(renderTiles);
}

function renderViewer(){
  if(!current || viewer.imageId !== current.id) return;
  clampViewer();
  $('analysis-canvas').style.width = current.width + 'px';
  $('analysis-canvas').style.height = current.height + 'px';
  $('analysis-canvas').style.transform = `translate(${viewer.x}px,${viewer.y}px) scale(${viewer.scale})`;
  $('zoom').value = String(Math.max(Number($('zoom').min), Math.min(Number($('zoom').max), viewer.scale * 100)));
  $('zoomvalue').textContent = `${Math.round(viewer.scale * 100)}%`;
  scheduleTiles();
}

function fitViewer(){
  if(!current) return;
  const stage = $('image-stage');
  viewer.scale = Math.min(1, Math.max(.001, Math.min((stage.clientWidth - 24) / current.width, (stage.clientHeight - 24) / current.height)));
  viewer.fit = true; viewer.x = 0; viewer.y = 0;
  $('zoom').min = String(Math.max(.1, viewer.scale * 50));
  renderViewer();
}

function setViewerZoom(next, anchorX, anchorY){
  if(!current) return;
  const stage = $('image-stage');
  const old = viewer.scale;
  next = Math.min(8, Math.max(Math.max(.001, Number($('zoom').min) / 100), next));
  const ax = anchorX ?? stage.clientWidth / 2, ay = anchorY ?? stage.clientHeight / 2;
  const imageX = (ax - viewer.x) / old, imageY = (ay - viewer.y) / old;
  viewer.scale = next; viewer.x = ax - imageX * next; viewer.y = ay - imageY * next; viewer.fit = false;
  renderViewer();
}

function renderViewerMode(){
  const filter = mode === 'negative' ? 'invert(1)' : mode === 'grayscale' ? 'grayscale(1)' : '';
  $('inspect-image').style.filter = filter;
  $('tile-layer').style.filter = filter;
  $('tile-layer').classList.toggle('hidden', mode === 'preview');
  if(mode === 'preview') $('pyramid-status').textContent = 'Prévia integral · visualização de inspeção';
  else scheduleTiles();
}

function renderTiles(){
  if(!current || viewer.imageId !== current.id || !viewer.manifest || mode === 'preview') return;
  const stage = $('image-stage'), layer = $('tile-layer'), levels = viewer.manifest.levels;
  const density = viewer.scale * (window.devicePixelRatio || 1);
  let level = levels[levels.length - 1];
  for(const candidate of levels){ if(candidate.width / current.width >= density * .85){ level = candidate; break; } }
  const left = Math.max(0, -viewer.x / viewer.scale), top = Math.max(0, -viewer.y / viewer.scale);
  const right = Math.min(current.width, (stage.clientWidth - viewer.x) / viewer.scale), bottom = Math.min(current.height, (stage.clientHeight - viewer.y) / viewer.scale);
  const tileSize = viewer.manifest.tile_size, factorX = current.width / level.width, factorY = current.height / level.height;
  const minX = Math.max(0, Math.floor(left / factorX / tileSize) - 1), minY = Math.max(0, Math.floor(top / factorY / tileSize) - 1);
  const maxX = Math.min(level.columns - 1, Math.floor(Math.max(0, right - 1) / factorX / tileSize) + 1), maxY = Math.min(level.rows - 1, Math.floor(Math.max(0, bottom - 1) / factorY / tileSize) + 1);
  const wanted = new Set();
  for(let y = minY; y <= maxY; y++) for(let x = minX; x <= maxX; x++){
    const key = `${level.level}:${x}:${y}`; wanted.add(key);
    if(layer.querySelector(`[data-tile="${key}"]`)) continue;
    const image = document.createElement('img'); image.dataset.tile = key; image.alt = ''; image.draggable = false;
    const lx = x * tileSize, ly = y * tileSize, lw = Math.min(tileSize, level.width - lx), lh = Math.min(tileSize, level.height - ly);
    image.style.left = `${lx * factorX}px`; image.style.top = `${ly * factorY}px`; image.style.width = `${lw * factorX + .25}px`; image.style.height = `${lh * factorY + .25}px`;
    image.onload = image.onerror = () => updateTileStatus(level);
    image.src = level.url_template.replace('{x}', x).replace('{y}', y);
    layer.append(image);
  }
  for(const image of Array.from(layer.children)) if(!wanted.has(image.dataset.tile)) image.remove();
  updateTileStatus(level);
}

function updateTileStatus(level){
  const tiles = Array.from($('tile-layer').children), loaded = tiles.filter(image => image.complete && image.naturalWidth).length;
  $('pyramid-status').textContent = `Pirâmide ${level.level + 1}/${viewer.manifest.levels.length} · ${loaded}/${tiles.length} blocos visíveis`;
  $('image-stage').classList.toggle('tiles-loading', loaded < tiles.length);
}

async function initializeViewer(imageId){
  const revision = ++viewer.revision;
  viewer.imageId = imageId; viewer.manifest = null; viewer.drag = null; viewer.fit = true;
  $('tile-layer').replaceChildren();
  $('inspect-image').src = `/api/v1/images/${imageId}/preview`;
  $('pyramid-status').textContent = 'Carregando pirâmide progressiva…';
  renderViewerMode();
  requestAnimationFrame(fitViewer);
  try{
    const manifest = await api(`/api/v1/images/${imageId}/pyramid`);
    if(revision !== viewer.revision || !current || current.id !== imageId) return;
    viewer.manifest = manifest; renderViewer();
  }catch(err){
    if(revision !== viewer.revision) return;
    $('pyramid-status').textContent = 'Prévia disponível · pirâmide indisponível'; toast(err.message);
  }
}

async function openInspect(id){
  current = images.find(x => x.id === id);
  if(!current) return;
  // MORFO thematic workspace is authoritative; do not auto-run or load legacy classifiers.
  mode = 'original';
  activeLayer = 'original';
  segCanvases = null;
  segCurrentRunId = null;
  currentAnalysis = null;
  analysisRuns = [];
  selectedRegion = null;
  $('inspect-title').textContent = current.filename;
  $('inspector').classList.remove('hidden');
  document.body.style.overflow = 'hidden';
  $('region-overlay').replaceChildren();
  $('region-overlay').style.display = 'none';
  $('segmented-layer').classList.add('hidden');
  $('tile-layer').style.display = 'block';
  $('inspect-image').style.display = 'block';
  initializeViewer(current.id);
  fillInspect();
  window.dispatchEvent(new CustomEvent('morfo:image-opened', {detail:{imageId:current.id}}));
  $('close-inspect').focus();
}

function fillInspect(){
  if(!current) return;
  for(const b of document.querySelectorAll('[data-image-mode]'))
    b.classList.toggle('active', b.dataset.imageMode === mode);
  renderViewerMode();
  $('inspect-metadata').innerHTML = [
    ['Identificador', current.id],
    ['Amostra', current.sample_id || 'Não informada'],
    ['Captura', current.capture_date || 'Não informada'],
    ['Dimensões', `${number.format(current.width)} × ${number.format(current.height)} px`],
    ['Formato', current.format.toUpperCase()],
    ['Tamanho', `${(current.bytes / 1024 / 1024).toFixed(2)} MiB`],
    ['Responsável', current.user_email || 'Não registrado'],
    ['Preparação', current.pipeline],
    ['Estado científico', 'RESULTADOS TEMÁTICOS NÃO VALIDADOS'],
    ['SHA-256', current.sha256],
    ['Arquivo Original', `<a href="/api/v1/images/${current.id}/original" download="${h(current.filename)}" class="open-link" style="display:inline-flex;align-items:center;gap:4px">Baixar arquivo original (${current.format.toUpperCase()}) ↗</a>`]
  ].map(([label, value], i) => `<div class="${i >= 9 ? 'hash' : ''}"><dt>${h(label)}</dt><dd>${value}</dd></div>`).join('');
}

function regionById(id){ return currentAnalysis?.regions?.find(r => r.id === id); }
function automaticLabel(label, isRecalibrated = false){
  if (isRecalibrated) {
    return label === 'ROOT' ? 'Possível raiz (recalibrada online)' : 'Desconhecida (recalibrada online)';
  }
  return label === 'ROOT' ? 'Possível raiz — não confirmada' : 'Estrutura desconhecida — revisão necessária';
}

function findNextRecommendedRegion(currentId){
  if(!currentAnalysis?.regions) return null;
  const pending = currentAnalysis.regions.filter(r => !r.review && r.id !== currentId);
  if(!pending.length) return null;
  pending.sort((a, b) => {
    const confA = a.judgment_evidence?.confidence ?? 0.5;
    const confB = b.judgment_evidence?.confidence ?? 0.5;
    return confA - confB;
  });
  return pending[0];
}

async function loadAnalysisRun(runId){
  try{
    const result=await api(`/api/v1/images/${current.id}/analysis?run_id=${encodeURIComponent(runId)}`);
    currentAnalysis=result.data;
    selectedRegion=currentAnalysis?.regions?.[0] || null;
    segCanvases=null;
    segCurrentRunId=null;
    renderAnalysis();fillInspect();
    if(selectedRegion){
      loadHistory(selectedRegion.id);
      renderSampleCrop(selectedRegion);
    }
  }catch(err){toast(err.message);}
}

function centerOnRegion(r) {
  if (!r || !current) return;
  const stage = $('image-stage');
  if (!stage) return;
  const targetX = r.x + r.width / 2;
  const targetY = r.y + r.height / 2;
  const minDesiredScale = Math.min(2.5, Math.max(0.85, (stage.clientWidth * 0.45) / Math.max(r.width, r.height)));
  if (viewer.scale < minDesiredScale) {
    viewer.scale = minDesiredScale;
  }
  viewer.fit = false;
  viewer.x = stage.clientWidth / 2 - targetX * viewer.scale;
  viewer.y = stage.clientHeight / 2 - targetY * viewer.scale;
  clampViewer();
  renderViewer();
}

function renderSampleCrop(region) {
  const container = $('sample-crop-canvas');
  if (!container || !region || !current) return;
  const ctx = container.getContext('2d');
  const cw = container.width, ch = container.height;
  ctx.clearRect(0, 0, cw, ch);

  const drawWithImg = (sourceImg) => {
    const swReal = sourceImg.naturalWidth || sourceImg.width || current.width;
    const shReal = sourceImg.naturalHeight || sourceImg.height || current.height;
    const scaleX = swReal / current.width;
    const scaleY = shReal / current.height;

    const rx = region.x * scaleX, ry = region.y * scaleY;
    const rw = region.width * scaleX, rh = region.height * scaleY;

    const padX = Math.max(20, rw * 0.45);
    const padY = Math.max(20, rh * 0.45);
    const sx = Math.max(0, rx - padX);
    const sy = Math.max(0, ry - padY);
    const sw = Math.min(swReal - sx, rw + padX * 2);
    const sh = Math.min(shReal - sy, rh + padY * 2);

    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = 'high';
    ctx.drawImage(sourceImg, sx, sy, sw, sh, 0, 0, cw, ch);

    const mapX = cw / sw;
    const mapY = ch / sh;
    const bx = (rx - sx) * mapX;
    const by = (ry - sy) * mapY;
    const bw = rw * mapX;
    const bh = rh * mapY;

    // Caixa de destaque da amostra sobre o recorte
    ctx.strokeStyle = '#00ffcc';
    ctx.lineWidth = 2.5;
    ctx.strokeRect(bx, by, bw, bh);

    // Tag
    ctx.fillStyle = '#07243e';
    ctx.fillRect(bx, Math.max(0, by - 20), 40, 19);
    ctx.strokeStyle = '#00ffcc';
    ctx.lineWidth = 1;
    ctx.strokeRect(bx, Math.max(0, by - 20), 40, 19);
    ctx.fillStyle = '#ffffff';
    ctx.font = 'bold 11px system-ui, sans-serif';
    ctx.fillText(`R${region.ordinal}`, bx + 5, Math.max(14, by - 6));
  };

  const inspectImg = $('inspect-image');
  if (inspectImg && inspectImg.complete && inspectImg.naturalWidth > 0) {
    drawWithImg(inspectImg);
  } else {
    const tempImg = new Image();
    tempImg.crossOrigin = 'anonymous';
    tempImg.onload = () => drawWithImg(tempImg);
    tempImg.src = `/api/v1/images/${encodeURIComponent(current.id)}/original-view`;
  }
}

function selectRegion(id, shouldCenter = true){
  selectedRegion = regionById(id);
  renderAnalysis();
  applyActiveLayer();
  if(selectedRegion){
    loadHistory(selectedRegion.id);
    if(shouldCenter) centerOnRegion(selectedRegion);
    renderSampleCrop(selectedRegion);
  }
}

let editingNotes = false;
let activeLayer = 'roots'; // 'roots' | 'matrix' | 'binary' | 'overlay' | 'boxes'
let segCanvases = null;
let segGenerating = false;
let segCurrentRunId = null;
let segSensitivity = 2.3;
let segScaleWidthCm = 20.0;
let segModelStats = null;

function recomputeSegmentation(sensitivity = segSensitivity) {
  if (!segModelStats) return;
  const { src, w, h, meanR, meanG, meanB, stdR, stdG, stdB, sMeanR, sMeanG, sMeanB, sStdR, sStdG, sStdB,
          rootsCanvas, matrixCanvas, binaryCanvas, overlayCanvas, rootsCtx, matrixCtx, binaryCtx, overlayCtx,
          rootsData, matrixData, binaryData, overlayData } = segModelStats;

  const rPix = rootsData.data, mPix = matrixData.data, bPix = binaryData.data, oPix = overlayData.data;
  const isRootPixel = new Uint8Array(w * h);
  const total = w * h;

  for (let i = 0; i < total; i++) {
    const idx = i * 4;
    const r = src[idx], g = src[idx + 1], b = src[idx + 2];
    const distRoot = Math.sqrt(
      Math.pow((r - meanR) / stdR, 2) +
      Math.pow((g - meanG) / stdG, 2) +
      Math.pow((b - meanB) / stdB, 2)
    );
    const distSoil = Math.sqrt(
      Math.pow((r - sMeanR) / sStdR, 2) +
      Math.pow((g - sMeanG) / sStdG, 2) +
      Math.pow((b - sMeanB) / sStdB, 2)
    );

    // Classificação orgânica pixel-a-pixel:
    // Nunca impõe retângulos rígidos; o contorno segue exatamente o filamento natural
    if (distRoot < sensitivity && distRoot < distSoil * 0.98) {
      isRootPixel[i] = 1;
    }
  }

  // Morfologia de continuidade: remove ruído isolado de 1px e conecta eixos radiculares
  const refinedMask = new Uint8Array(w * h);
  let totalRootPx = 0;
  for (let y = 1; y < h - 1; y++) {
    const row = y * w;
    for (let x = 1; x < w - 1; x++) {
      const i = row + x;
      const neighbors = isRootPixel[i - 1] + isRootPixel[i + 1] + isRootPixel[i - w] + isRootPixel[i + w];
      if (isRootPixel[i]) {
        // Preservar apenas se conectado a filamento (ignora ruídos isolados no solo)
        if (neighbors >= 1) {
          refinedMask[i] = 1;
          totalRootPx++;
        }
        continue;
      }
      // Conectar quebras em filamentos finos
      if (neighbors >= 2) {
        const idx = i * 4;
        const r = src[idx], g = src[idx + 1], b = src[idx + 2];
        const distRoot = Math.sqrt(Math.pow((r - meanR) / stdR, 2) + Math.pow((g - meanG) / stdG, 2) + Math.pow((b - meanB) / stdB, 2));
        if (distRoot < sensitivity + 0.35) {
          refinedMask[i] = 1;
          totalRootPx++;
        }
      }
    }
  }

  // Montagem das 4 camadas científicas globais
  for (let i = 0; i < total; i++) {
    const idx = i * 4;
    const r = src[idx], g = src[idx + 1], b = src[idx + 2];
    if (refinedMask[i]) {
      // Camada 'roots': Apenas os pixels orgânicos de raiz, fundo transparente
      rPix[idx] = r; rPix[idx + 1] = g; rPix[idx + 2] = b; rPix[idx + 3] = 255;
      // Camada 'matrix': Solo original com raízes subtraídas (fundo branco limpo no lugar da raiz)
      mPix[idx] = 255; mPix[idx + 1] = 255; mPix[idx + 2] = 255; mPix[idx + 3] = 255;
      // Camada 'binary': Máscara P&B (Branco = Raiz, Preto = Solo)
      bPix[idx] = 255; bPix[idx + 1] = 255; bPix[idx + 2] = 255; bPix[idx + 3] = 255;
      // Camada 'overlay': Foto com realce fluorescente bio-verde
      oPix[idx] = Math.round(r * 0.35);
      oPix[idx + 1] = Math.min(255, Math.round(g * 1.35 + 50));
      oPix[idx + 2] = Math.round(b * 0.45);
      oPix[idx + 3] = 255;
    } else {
      rPix[idx] = 0; rPix[idx + 1] = 0; rPix[idx + 2] = 0; rPix[idx + 3] = 0;
      mPix[idx] = r; mPix[idx + 1] = g; mPix[idx + 2] = b; mPix[idx + 3] = 255;
      bPix[idx] = 0; bPix[idx + 1] = 0; bPix[idx + 2] = 0; bPix[idx + 3] = 255;
      oPix[idx] = r; oPix[idx + 1] = g; oPix[idx + 2] = b; oPix[idx + 3] = 255;
    }
  }

  rootsCtx.putImageData(rootsData, 0, 0);
  matrixCtx.putImageData(matrixData, 0, 0);
  binaryCtx.putImageData(binaryData, 0, 0);
  overlayCtx.putImageData(overlayData, 0, 0);

  const pxPerCm = w / segScaleWidthCm;
  const faceAreaCm2 = segScaleWidthCm * (h / pxPerCm);
  const maskAreaCm2 = (totalRootPx / (pxPerCm * pxPerCm)).toFixed(2);
  const maskCoveragePct = ((totalRootPx / Math.max(1, total)) * 100).toFixed(2);

  segCanvases = {
    roots: rootsCanvas, matrix: matrixCanvas, binary: binaryCanvas, overlay: overlayCanvas,
    w, h, totalRootPx, faceAreaCm2, maskAreaCm2, maskCoveragePct
  };
  segCurrentRunId = currentAnalysis ? currentAnalysis.id : null;

  if ($('scale-density-val')) {
    $('scale-density-val').textContent = `${maskAreaCm2} cm² (${maskCoveragePct}% da imagem)`;
  }

  applyActiveLayer();
}

async function prepareSegmentationCanvases(force = false) {
  if (!current || !currentAnalysis) return;
  if (segCanvases && segCurrentRunId === currentAnalysis.id && !force) return;
  if (segGenerating) return;

  segGenerating = true;
  try {
    const img = new Image();
    img.crossOrigin = 'anonymous';
    await new Promise((resolve, reject) => {
      img.onload = resolve;
      img.onerror = () => reject(new Error('Falha ao carregar imagem para segmentação'));
      img.src = `/api/v1/images/${encodeURIComponent(current.id)}/original-view`;
    });

    const w = img.naturalWidth || current.width || 800;
    const h = img.naturalHeight || current.height || 600;

    const baseCanvas = document.createElement('canvas');
    baseCanvas.width = w; baseCanvas.height = h;
    const baseCtx = baseCanvas.getContext('2d');
    baseCtx.drawImage(img, 0, 0, w, h);
    const baseData = baseCtx.getImageData(0, 0, w, h);
    const src = baseData.data;

    const rootsCanvas = document.createElement('canvas');
    rootsCanvas.width = w; rootsCanvas.height = h;
    const rootsCtx = rootsCanvas.getContext('2d');
    const rootsData = rootsCtx.createImageData(w, h);

    const matrixCanvas = document.createElement('canvas');
    matrixCanvas.width = w; matrixCanvas.height = h;
    const matrixCtx = matrixCanvas.getContext('2d');
    const matrixData = matrixCtx.createImageData(w, h);

    const binaryCanvas = document.createElement('canvas');
    binaryCanvas.width = w; binaryCanvas.height = h;
    const binaryCtx = binaryCanvas.getContext('2d');
    const binaryData = binaryCtx.createImageData(w, h);

    const overlayCanvas = document.createElement('canvas');
    overlayCanvas.width = w; overlayCanvas.height = h;
    const overlayCtx = overlayCanvas.getContext('2d');
    const overlayData = overlayCtx.createImageData(w, h);

    // 1. Amostragem Fina: Aprender a distribuição espectral de raiz e solo sem forçar blocos retangulares
    let sumR = 0, sumG = 0, sumB = 0, rootPixCount = 0;
    let sqR = 0, sqG = 0, sqB = 0;

    let soilSumR = 0, soilSumG = 0, soilSumB = 0, soilCount = 0;
    let soilSqR = 0, soilSqG = 0, soilSqB = 0;

    // Amostragem de fundo geral do solo do monolito (grade distribuída)
    const step = Math.max(1, Math.floor((w * h) / 10000));
    for (let i = 0; i < w * h; i += step) {
      const idx = i * 4;
      const pr = src[idx], pg = src[idx + 1], pb = src[idx + 2];
      soilSumR += pr; soilSumG += pg; soilSumB += pb;
      soilSqR += pr * pr; soilSqG += pg * pg; soilSqB += pb * pb;
      soilCount++;
    }

    const regions = currentAnalysis.regions || [];
    for (const r of regions) {
      const label = r.review?.label || r.suggestion;
      const x0 = Math.max(0, Math.floor(r.x)), y0 = Math.max(0, Math.floor(r.y));
      const x1 = Math.min(w, Math.ceil(r.x + r.width)), y1 = Math.min(h, Math.ceil(r.y + r.height));
      const boxW = x1 - x0, boxH = y1 - y0;
      if (boxW <= 0 || boxH <= 0) continue;

      if (label === 'ROOT') {
        // Encontrar média de luminância na vizinhança da caixa para isolar o filamento
        let localSum = 0, count = 0;
        for (let y = y0; y < y1; y++) {
          const row = y * w;
          for (let x = x0; x < x1; x++) {
            const idx = (row + x) * 4;
            localSum += 0.299 * src[idx] + 0.587 * src[idx + 1] + 0.114 * src[idx + 2];
            count++;
          }
        }
        const localMean = count > 0 ? (localSum / count) : 128;

        // Amostrar apenas os pixels que realmente se destacam como filamento radicular dentro da caixa!
        // Os demais pixels dentro da caixa permanecem como solo.
        for (let y = y0; y < y1; y++) {
          const row = y * w;
          for (let x = x0; x < x1; x++) {
            const idx = (row + x) * 4;
            const pr = src[idx], pg = src[idx + 1], pb = src[idx + 2];
            const pGray = 0.299 * pr + 0.587 * pg + 0.114 * pb;
            if (pGray > localMean + 3) {
              sumR += pr; sumG += pg; sumB += pb;
              sqR += pr * pr; sqG += pg * pg; sqB += pb * pb;
              rootPixCount++;
            } else {
              soilSumR += pr; soilSumG += pg; soilSumB += pb;
              soilSqR += pr * pr; soilSqG += pg * pg; soilSqB += pb * pb;
              soilCount++;
            }
          }
        }
      } else if (label === 'MATRIX' || label === 'ARTIFACT') {
        // Amostra confirmada como solo ou artefato: calibra distribuição negativa
        for (let y = y0; y < y1; y++) {
          const row = y * w;
          for (let x = x0; x < x1; x++) {
            const idx = (row + x) * 4;
            const pr = src[idx], pg = src[idx + 1], pb = src[idx + 2];
            soilSumR += pr * 2; soilSumG += pg * 2; soilSumB += pb * 2;
            soilSqR += pr * pr * 2; soilSqG += pg * pg * 2; soilSqB += pb * pb * 2;
            soilCount += 2;
          }
        }
      }
    }

    const nRoot = Math.max(1, rootPixCount), nSoil = Math.max(1, soilCount);
    const meanR = sumR / nRoot, meanG = sumG / nRoot, meanB = sumB / nRoot;
    const stdR = Math.max(10, Math.sqrt(Math.max(0, sqR / nRoot - meanR * meanR)));
    const stdG = Math.max(10, Math.sqrt(Math.max(0, sqG / nRoot - meanG * meanG)));
    const stdB = Math.max(10, Math.sqrt(Math.max(0, sqB / nRoot - meanB * meanB)));

    const sMeanR = soilSumR / nSoil, sMeanG = soilSumG / nSoil, sMeanB = soilSumB / nSoil;
    const sStdR = Math.max(10, Math.sqrt(Math.max(0, soilSqR / nSoil - sMeanR * sMeanR)));
    const sStdG = Math.max(10, Math.sqrt(Math.max(0, soilSqG / nSoil - sMeanG * sMeanG)));
    const sStdB = Math.max(10, Math.sqrt(Math.max(0, soilSqB / nSoil - sMeanB * sMeanB)));

    segModelStats = {
      src, w, h,
      meanR, meanG, meanB, stdR, stdG, stdB,
      sMeanR, sMeanG, sMeanB, sStdR, sStdG, sStdB,
      rootsCanvas, matrixCanvas, binaryCanvas, overlayCanvas,
      rootsCtx, matrixCtx, binaryCtx, overlayCtx,
      rootsData, matrixData, binaryData, overlayData
    };

    recomputeSegmentation(segSensitivity);
  } catch (err) {
    console.error('Falha ao processar segmentação:', err);
  } finally {
    segGenerating = false;
  }
}

function applyActiveLayer() {
  const segLayer = $('segmented-layer');
  const regionOverlay = $('region-overlay');
  const tileLayer = $('tile-layer');
  const inspectImg = $('inspect-image');
  if (!segLayer) return;

  // Sincronizar classes ativas tanto no topo quanto no painel lateral
  for (const b of document.querySelectorAll('[data-layer-btn], [data-top-layer]')) {
    const l = b.dataset.layerBtn || b.dataset.topLayer;
    b.classList.toggle('active', l === activeLayer);
  }

  // Visibilidade das caixas:
  // Se for 'boxes', exibe todas as caixas.
  // Se houver uma amostra selecionada (selectedRegion), exibe a caixa dela em destaque em qualquer camada!
  if (regionOverlay) {
    if (activeLayer === 'boxes') {
      regionOverlay.style.display = 'block';
      for (const box of regionOverlay.children) box.style.display = 'block';
    } else if (selectedRegion) {
      regionOverlay.style.display = 'block';
      for (const box of regionOverlay.children) {
        box.style.display = (box.dataset.regionBox === selectedRegion.id) ? 'block' : 'none';
      }
    } else {
      regionOverlay.style.display = 'none';
    }
  }

  // Camada 1: Original Intacta e Preservada
  if (activeLayer === 'original') {
    segLayer.classList.add('hidden');
    if (tileLayer) tileLayer.style.display = 'block';
    if (inspectImg) inspectImg.style.display = 'block';
    if ($('pyramid-status')) $('pyramid-status').textContent = '📷 Imagem Original Preservada (Intacta)';
    return;
  }

  // Camada de Caixas / Fragmentos de Calibração
  if (activeLayer === 'boxes' || !segCanvases) {
    segLayer.classList.add('hidden');
    if (tileLayer) tileLayer.style.display = 'block';
    if (inspectImg) inspectImg.style.display = 'block';
    if ($('pyramid-status')) $('pyramid-status').textContent = 'Modo de calibração: Fragmentos/Amostras iniciais';
    return;
  }

  // Camadas de Segmentação Densa de Toda a Imagem
  segLayer.width = segCanvases.w;
  segLayer.height = segCanvases.h;
  const ctx = segLayer.getContext('2d');
  ctx.clearRect(0, 0, segCanvases.w, segCanvases.h);

  const selected = segCanvases[activeLayer];
  if (selected) {
    ctx.drawImage(selected, 0, 0);
  }
  segLayer.classList.remove('hidden');

  if (activeLayer === 'roots' || activeLayer === 'matrix' || activeLayer === 'binary') {
    if (tileLayer) tileLayer.style.display = 'none';
    if (inspectImg) inspectImg.style.display = 'none';
  } else {
    if (tileLayer) tileLayer.style.display = 'block';
    if (inspectImg) inspectImg.style.display = 'block';
  }

  const layerNames = {
    original: '📷 Imagem Original Preservada (Intacta)',
    roots: '🌿 Somente Raízes Reconhecidas em Toda a Imagem (Isolamento Radicular)',
    matrix: '🟤 Matriz de Solo em Toda a Imagem (Raízes Subtraídas)',
    binary: '⬛ Máscara Binária Científica P&B (Padrão ImageJ / RhizoVision)',
    overlay: '🔍 Sobreposição Nítida em Toda a Imagem (Foto + Raízes)',
    boxes: '📦 Fragmentos de Calibração'
  };
  if ($('pyramid-status')) {
    $('pyramid-status').textContent = `Camada Ativa: ${layerNames[activeLayer] || activeLayer}`;
  }
}

function downloadLayerPng(layerKey, label){
  if(!segCanvases || !segCanvases[layerKey]){
    toast('Camada ainda sendo gerada...');
    return;
  }
  const canvas = segCanvases[layerKey];
  canvas.toBlob((blob) => {
    if(!blob) return;
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `rizoma-${encodeURIComponent(current.sample_id || current.id)}-${label}.png`;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
    toast(`Arquivo ${a.download} exportado!`);
  }, 'image/png');
}

async function validateFinalAnalysis(){
  if(!currentAnalysis) return;
  try{
    const res = await api(`/api/v1/analysis-runs/${currentAnalysis.id}`, {
      method: 'PATCH',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({
        status: 'COMPLETED',
        notes: (currentAnalysis.notes ? currentAnalysis.notes + '\n\n' : '') + '✓ Revisão da segmentação concluída pelo operador.'
      })
    });
    currentAnalysis.status = res.data.status;
    currentAnalysis.notes = res.data.notes;
    currentAnalysis.completed_at = res.data.completed_at;
    const item = analysisRuns.find(r => r.id === currentAnalysis.id);
    if(item) item.status = res.data.status;
    renderAnalysis();
    toast('Revisão concluída. Estado científico permanece experimental.');
  }catch(err){ toast(err.message); }
}

function renderAnalysis(){
  const panel = $('analysis-panel'), overlay = $('region-overlay');
  const topLayerGroup = $('layer-segmented-top');
  const topStartBtn = $('start-analysis');

  if(!currentAnalysis){
    if (topLayerGroup) topLayerGroup.classList.add('hidden');
    if (topStartBtn) topStartBtn.textContent = '⚡ Analisar experimentalmente';
    overlay.innerHTML = '';
    panel.innerHTML = `
      <div class="analysis-empty-card">
        <div class="empty-badge">PREPARAÇÃO CIENTÍFICA</div>
        <h3>Iniciar Processamento e Análise</h3>
        <p>A imagem do monolito está preservada em formato original intacto com verificação criptográfica SHA-256. Execute a análise para identificar amostras de calibração e treinar o modelo de segmentação densa de toda a imagem.</p>
        
        <div class="analysis-config-box">
          <label class="config-label" for="analysis-strategy-empty">
            <span style="font-size:0.73rem;font-weight:750;color:var(--navy)">Estratégia de Aprendizado:</span>
            <select id="analysis-strategy-empty" class="select-strategy-panel">
              <option value="BAYESIAN_CALIBRATED" selected>Nível 1: Calibração Bayesiana (Recomendado)</option>
              <option value="TABULAR_INCREMENTAL">Nível 2: Classificador Tabular (Online ML)</option>
              <option value="VISUAL_KNN">Nível 3: Similaridade Visual k-NN &amp; Active Learning</option>
              <option value="BASELINE_HEURISTIC">Baseline Heurístico (Estático)</option>
            </select>
          </label>
          <div class="strategy-desc-banner" id="strategy-desc-banner">
            ${getStrategyDescHtml('BAYESIAN_CALIBRATED')}
          </div>

          <div class="analysis-feature-list">
            <div class="feat-item">
              <span class="feat-icon">🔬</span>
              <div>
                <strong>Amostragem Inteligente</strong>
                <small>Detecta eixos radiculares e matriz de solo para treinamento</small>
              </div>
            </div>
            <div class="feat-item">
              <span class="feat-icon">🌱</span>
              <div>
                <strong>Classificação Pixel-a-Pixel</strong>
                <small>Aprende o perfil estatístico e gera as camadas de raízes, solo e máscara</small>
              </div>
            </div>
            <div class="feat-item">
              <span class="feat-icon">🔒</span>
              <div>
                <strong>Custódia Criptográfica</strong>
                <small>O arquivo original nunca é alterado ou sobrescrito</small>
              </div>
            </div>
          </div>

          <button type="button" class="button primary full btn-start-hero" id="btn-start-analysis-empty">
            ⚡ Executar Análise e Treinar Modelo
          </button>
        </div>
      </div>
    `;

    const stratSelect = $('analysis-strategy-empty');
    if (stratSelect) {
      stratSelect.onchange = e => {
        const banner = $('strategy-desc-banner');
        if (banner) banner.innerHTML = getStrategyDescHtml(e.target.value);
        if ($('analysis-strategy')) $('analysis-strategy').value = e.target.value;
      };
    }
    if ($('analysis-strategy')) {
      $('analysis-strategy').onchange = e => {
        if ($('analysis-strategy-empty')) $('analysis-strategy-empty').value = e.target.value;
        const banner = $('strategy-desc-banner');
        if (banner) banner.innerHTML = getStrategyDescHtml(e.target.value);
      };
    }

    const btnEmpty = $('btn-start-analysis-empty');
    if (btnEmpty) {
      btnEmpty.onclick = () => {
        if (stratSelect && $('analysis-strategy')) {
          $('analysis-strategy').value = stratSelect.value;
        }
        startAnalysis();
      };
    }
    return;
  }

  if (topLayerGroup) topLayerGroup.classList.remove('hidden');
  if (topStartBtn) topStartBtn.textContent = '↻ Re-analisar';

  const regions = currentAnalysis.regions || [];
  overlay.innerHTML = regions.map(r => {
    const isRecal = !r.review && r.judgment_evidence?.recalibrated;
    const isSelected = selectedRegion?.id === r.id;
    const cls = `region-box ${isSelected ? 'selected' : ''} ${r.suggestion === 'ROOT' ? 'root' : 'unknown'} ${isRecal ? 'recalibrated' : ''}`;
    const title = `Região ${r.ordinal}: ${h(automaticLabel(r.suggestion, isRecal))}${isRecal ? ' ⚡ Recalibrada em tempo real' : ''}`;
    const tag = isSelected ? `<span class="region-box-tag">Amostra R${r.ordinal}</span>` : '';
    return `<button type="button" class="${cls}" data-region-box="${h(r.id)}" style="left:${r.x}px;top:${r.y}px;width:${r.width}px;height:${r.height}px" title="${title}">${tag}</button>`;
  }).join('');
  for(const box of overlay.querySelectorAll('[data-region-box]')) box.onclick = () => selectRegion(box.dataset.regionBox);
  const selected = selectedRegion && regionById(selectedRegion.id);

  const st = currentAnalysis.status || 'REVIEW_REQUIRED';
  const statusLabel = st === 'COMPLETED' ? '✓ Revisão concluída' : (st === 'ARCHIVED' ? '⊘ Arquivada' : '⚡ Revisão Pendente');
  const statusCls = st === 'COMPLETED' ? 'completed' : (st === 'ARCHIVED' ? 'archived' : 'review');

  const notesHtml = editingNotes ? `
    <form id="notes-form" class="notes-edit-form">
      <textarea id="notes-textarea" placeholder="Observações científicas, notas metodológicas ou justificativas...">${h(currentAnalysis.notes || '')}</textarea>
      <div class="notes-edit-buttons">
        <button type="button" class="btn-subtle" id="btn-cancel-notes">Cancelar</button>
        <button type="submit" class="btn-subtle" style="background:var(--blue);color:#fff">Salvar notas</button>
      </div>
    </form>
  ` : `
    <div class="notes-content">${currentAnalysis.notes ? h(currentAnalysis.notes) : '<span class="notes-empty">Nenhuma observação registrada nesta análise.</span>'}</div>
  `;

  const stats = currentAnalysis.statistics || (() => {
    let root_count = 0, matrix_count = 0, artifact_count = 0, unknown_count = 0, reviewed_count = 0;
    let total_root_area = 0, total_region_area = 0;
    for (const r of regions) {
      const finalLabel = r.review?.label || r.suggestion;
      if (r.review) reviewed_count++;
      if (finalLabel === 'ROOT') { root_count++; total_root_area += (r.area || 0); }
      else if (finalLabel === 'MATRIX') matrix_count++;
      else if (finalLabel === 'ARTIFACT') artifact_count++;
      else unknown_count++;
      total_region_area += (r.area || 0);
    }
    return {
      total_regions: regions.length,
      root_count, matrix_count, artifact_count, unknown_count,
      reviewed_count, pending_count: regions.length - reviewed_count,
      total_root_area, total_region_area
    };
  })();

  const imgArea = (current?.width && current?.height) ? (current.width * current.height) : 0;
  const densityPct = imgArea > 0 ? ((stats.total_root_area / imgArea) * 100) : 0;
  const revPct = stats.total_regions > 0 ? Math.round((stats.reviewed_count / stats.total_regions) * 100) : 0;

  const segmentationCardHtml = `
    <div class="segmentation-card">
      <div class="segmentation-head">
        <span>MODELO CONFIRMADO · SEGMENTAÇÃO GLOBAL</span>
        <strong>Camadas Científicas do Monolito</strong>
      </div>
      <p class="segmentation-desc">Aplicação pixel-a-pixel sobre toda a imagem para isolamento fenotípico de raízes e solo.</p>
      
      <div class="layer-selector">
        <button type="button" class="btn-layer ${activeLayer==='original'?'active':''}" data-layer-btn="original" title="Exibe a imagem original preservada intacta">📷 Original</button>
        <button type="button" class="btn-layer ${activeLayer==='roots'?'active':''}" data-layer-btn="roots" title="Exibe exclusivamente as raízes reconhecidas">🌿 Raízes</button>
        <button type="button" class="btn-layer ${activeLayer==='matrix'?'active':''}" data-layer-btn="matrix" title="Exibe a matriz de solo com as raízes subtraídas">🟤 Solo</button>
        <button type="button" class="btn-layer ${activeLayer==='binary'?'active':''}" data-layer-btn="binary" title="Máscara preto e branco padrão ImageJ / RhizoVision">⬛ Máscara P&B</button>
        <button type="button" class="btn-layer ${activeLayer==='overlay'?'active':''}" data-layer-btn="overlay" title="Foto original com raízes destacadas sem caixas">🔍 Sobreposição</button>
        <button type="button" class="btn-layer ${activeLayer==='boxes'?'active':''}" data-layer-btn="boxes" title="Visualizar caixas de calibração">📦 Caixas</button>
      </div>

      <div class="sensitivity-control">
        <div class="sens-header">
          <span>Sensibilidade do Modelo:</span>
          <strong id="sens-val">${segSensitivity.toFixed(1)}x</strong>
        </div>
        <input type="range" id="sens-slider" min="1.0" max="4.0" step="0.1" value="${segSensitivity.toFixed(1)}" title="Ajuste fino de sensibilidade radicular">
        <small class="sens-hint">Ajusta em tempo real a detecção de ramificações finas vs. matriz de solo.</small>
      </div>

      <div class="scale-control">
        <div class="scale-row">
          <label for="scale-cm"><span>Largura do Monolito:</span></label>
          <div><input type="number" id="scale-cm" min="1" max="500" value="${segScaleWidthCm}" step="0.5"> <small>cm</small></div>
        </div>
        <div class="scale-result">
          <span>Área e cobertura da máscara:</span>
          <strong id="scale-density-val">${segCanvases?.maskAreaCm2 ? `${segCanvases.maskAreaCm2} cm² (${segCanvases.maskCoveragePct}% da imagem)` : 'Calculando…'}</strong>
          <small>Medida geométrica experimental; não representa comprimento radicular.</small>
        </div>
      </div>

      <div class="layer-downloads">
        <span class="layer-down-title">Exportar camadas e máscaras:</span>
        <div class="down-btn-grid">
          <button type="button" class="btn-down" id="dl-roots-png" title="Download da imagem contendo exclusivamente as raízes">🌿 Raízes (.PNG)</button>
          <button type="button" class="btn-down" id="dl-matrix-png" title="Download da imagem da matriz de solo">🟤 Solo (.PNG)</button>
          <button type="button" class="btn-down" id="dl-binary-png" title="Download da máscara binária para ImageJ/Rhizo">⬛ Máscara (.PNG)</button>
          <button type="button" class="btn-down" id="dl-full-png" title="Download da composição com sumário agronômico">🖼 Composição</button>
        </div>
      </div>

      <div class="validation-section">
        ${st === 'COMPLETED' ? `
          <div class="val-approved-badge">
            <span>✓ REVISÃO CONCLUÍDA PELO OPERADOR</span>
            <small>Segmentação experimental revisada; sem validação biológica implícita</small>
          </div>
        ` : `
          <div class="val-actions-row">
            <button type="button" class="btn-validate-final" id="btn-validate-final" title="Registrar conclusão da revisão operacional da segmentação">
              ✓ Concluir Revisão da Análise
            </button>
          </div>
        `}
      </div>
    </div>
  `;

  const statsHtml = `
    <div class="stats-summary-card">
      <div class="stats-card-head">
        <span>SUMÁRIO ESTATÍSTICO</span>
        <strong>Estatísticas Radiculares</strong>
      </div>
      <div class="stats-metrics-grid">
        <div class="stat-mini">
          <span class="stat-mini-label">🌱 Raízes</span>
          <strong class="stat-mini-val">${stats.root_count}</strong>
          <small>${number.format(stats.total_root_area)} px²</small>
        </div>
        <div class="stat-mini">
          <span class="stat-mini-label">📐 Densidade</span>
          <strong class="stat-mini-val">${densityPct.toFixed(3)}%</strong>
          <small>área da imagem</small>
        </div>
        <div class="stat-mini">
          <span class="stat-mini-label">⚖️ Solo / Artefatos</span>
          <strong class="stat-mini-val">${stats.matrix_count} / ${stats.artifact_count}</strong>
          <small>estruturas</small>
        </div>
        <div class="stat-mini">
          <span class="stat-mini-label">🎯 Revisão</span>
          <strong class="stat-mini-val">${revPct}%</strong>
          <small>${stats.reviewed_count}/${stats.total_regions} concluídas</small>
        </div>
      </div>
    </div>
    <div class="export-actions-group">
      <a href="/api/v1/images/${encodeURIComponent(current.id)}/analysis-export.csv?run_id=${encodeURIComponent(currentAnalysis.id)}" download="rizoma-analise-${encodeURIComponent(current.sample_id || current.id)}.csv" class="btn-export-csv" id="btn-export-stats-csv" style="grid-column:1/-1;padding:9px" title="Download de estatísticas e relatório RFC-4180 em CSV para R/Pandas/Excel">
        📊 Baixar Relatório Científico Completo (.CSV)
      </a>
    </div>
  `;

  panel.innerHTML = `<div class="analysis-head">
      <div>
        <span class="section-kicker">ANÁLISE EXPERIMENTAL</span>
        <strong>${h(currentAnalysis.algorithm_version)}</strong>
      </div>
      <div style="display:flex;gap:6px;align-items:center">
        <span class="status-badge ${statusCls}">${statusLabel}</span>
        <span class="analysis-count">${number.format(regions.length)} regiões</span>
      </div>
    </div>
    <div class="strategy-desc-banner" id="strategy-desc-banner-active" style="margin:8px 0 10px">
      ${getStrategyDescHtml(currentAnalysis.algorithm_version || $('analysis-strategy')?.value || 'BAYESIAN_CALIBRATED')}
    </div>
    <label class="run-picker">Execução preservada
      <select id="analysis-run-picker">${analysisRuns.map((run,index) => `<option value="${h(run.id)}" ${run.id===currentAnalysis.id?'selected':''}>${index+1}. ${h(run.created_at)} (${h(run.status || 'REVIEW')}) · ${number.format(run.region_count)} regiões</option>`).join('')}</select>
    </label>
    <div class="analysis-actions-row">
      ${st !== 'COMPLETED' ? `<button type="button" class="btn-action-small success" id="btn-complete-analysis" title="Concluir ciclo de revisão">✓ Concluir análise</button>` : ''}
      ${st !== 'ARCHIVED' ? `<button type="button" class="btn-action-small" id="btn-archive-analysis" title="Arquivar análise">⊘ Arquivar</button>` : ''}
      <button type="button" class="btn-action-small danger" id="btn-delete-analysis" title="Excluir execução de análise">🗑 Excluir análise</button>
    </div>
    ${regions.filter(r => !r.review).length > 0 ? `<button type="button" class="button primary full" id="btn-batch-confirm" style="margin:8px 0 10px;background:linear-gradient(135deg,var(--teal),var(--blue));font-size:0.82rem;padding:9px 12px">⚡ Confirmar todas pendentes (${regions.filter(r => !r.review).length})</button>` : (regions.length > 0 ? `<div style="background:#eaf7f2;color:#116e4b;border:1px solid #b7e6cf;padding:6px 10px;border-radius:6px;font-size:0.75rem;font-weight:700;margin:8px 0;display:flex;align-items:center;gap:6px">✓ Todas as ${regions.length} regiões confirmadas!</div>` : '')}
    ${segmentationCardHtml}
    ${statsHtml}
    <div class="analysis-notes-block">
      <div class="notes-header">
        <span>Notas científicas</span>
        ${!editingNotes ? `<button type="button" class="btn-subtle" id="btn-edit-notes" style="font-size:0.68rem;padding:2px 7px">✏ Editar</button>` : ''}
      </div>
      ${notesHtml}
    </div>
    <p class="analysis-warning">Sugestões não validadas biologicamente. Coordenadas referem-se ao original SHA-256 <code>${h(currentAnalysis.source_sha256.slice(0,12))}…</code></p>
    <div class="region-list">${regions.length ? regions.map(r => {
      const isRecal = !r.review && r.judgment_evidence?.recalibrated;
      const badgePill = isRecal ? '<span class="pill-recalibrated">⚡ Em tempo real</span>' : '';
      const subText = r.review
        ? `Decisão humana v${r.version}`
        : (isRecal ? 'Hipótese atualizada pós-feedback' : 'Sugestão automática inicial');
      return `<button type="button" data-region-row="${h(r.id)}" class="region-row ${selected?.id === r.id ? 'selected' : ''} ${r.review ? 'reviewed' : (isRecal ? 'recalibrated' : '')}">
        <span>R${r.ordinal}</span>
        <div>
          <strong>${h(r.review?.label || automaticLabel(r.suggestion, isRecal))} ${badgePill}</strong>
          <small>${subText} · ${number.format(r.area)} px candidatos</small>
        </div>
      </button>`;
    }).join('') : '<p class="no-regions">Nenhuma região candidata foi encontrada com estes parâmetros.</p>'}</div>
    ${selected ? reviewEditor(selected) : '<p class="choose-region">Selecione uma região na imagem ou na lista para revisar.</p>'}`;

  for(const row of panel.querySelectorAll('[data-region-row]')) row.onclick = () => selectRegion(row.dataset.regionRow);
  if($('analysis-run-picker')) $('analysis-run-picker').onchange = e => loadAnalysisRun(e.target.value);
  if($('review-form')) $('review-form').onsubmit = (e) => saveReview(e, false);
  if($('btn-next-review')) $('btn-next-review').onclick = (e) => saveReview(e, true);

  const reviewLabel = $('review-label');
  const reviewAction = $('review-action');
  if (reviewLabel && reviewAction && selected) {
    const curHypo = selected.review?.label || selected.suggestion;
    reviewLabel.onchange = () => {
      if (reviewLabel.value !== curHypo) {
        reviewAction.value = 'CORRECT';
      } else {
        reviewAction.value = curHypo === 'UNKNOWN' ? 'CORRECT' : 'CONFIRM';
      }
    };
  }

  if ($('analysis-strategy')) {
    $('analysis-strategy').onchange = e => {
      const banner = $('strategy-desc-banner-active') || $('strategy-desc-banner');
      if (banner) banner.innerHTML = getStrategyDescHtml(e.target.value);
      if ($('analysis-strategy-empty')) $('analysis-strategy-empty').value = e.target.value;
      const s = STRATEGY_DESCRIPTIONS[e.target.value];
      if (s) $('analysis-strategy').title = `${s.name} — ${s.summary}`;
    };
  }

  // Botões de camada do painel lateral
  for(const b of panel.querySelectorAll('[data-layer-btn]')){
    b.onclick = () => {
      activeLayer = b.dataset.layerBtn;
      applyActiveLayer();
    };
  }

  // Botões de camada da barra superior
  for(const b of document.querySelectorAll('[data-top-layer]')){
    b.onclick = () => {
      activeLayer = b.dataset.topLayer;
      applyActiveLayer();
    };
  }

  // Controles interativos de sensibilidade e escala
  if($('sens-slider')){
    $('sens-slider').oninput = e => {
      segSensitivity = parseFloat(e.target.value);
      if($('sens-val')) $('sens-val').textContent = `${segSensitivity.toFixed(1)}x`;
      recomputeSegmentation(segSensitivity);
    };
  }

  if($('scale-cm')){
    $('scale-cm').onchange = e => {
      segScaleWidthCm = Math.max(1, parseFloat(e.target.value) || 20);
      recomputeSegmentation(segSensitivity);
    };
  }

  // Botões de download de imagens de camada
  if($('dl-roots-png')) $('dl-roots-png').onclick = () => downloadLayerPng('roots', 'somente-raizes');
  if($('dl-matrix-png')) $('dl-matrix-png').onclick = () => downloadLayerPng('matrix', 'matriz-solo');
  if($('dl-binary-png')) $('dl-binary-png').onclick = () => downloadLayerPng('binary', 'mascara-binaria');
  if($('dl-full-png')) $('dl-full-png').onclick = () => exportClassifiedImage();

  // Botão de validação final
  if($('btn-validate-final')) $('btn-validate-final').onclick = () => validateFinalAnalysis();

  if($('btn-complete-analysis')) $('btn-complete-analysis').onclick = () => setAnalysisStatus('COMPLETED');
  if($('btn-archive-analysis')) $('btn-archive-analysis').onclick = () => setAnalysisStatus('ARCHIVED');
  if($('btn-delete-analysis')) $('btn-delete-analysis').onclick = () => openDeleteAnalysisModal();
  if($('btn-batch-confirm')) $('btn-batch-confirm').onclick = () => openBatchConfirmModal();
  if($('btn-edit-notes')) $('btn-edit-notes').onclick = () => { editingNotes = true; renderAnalysis(); };
  if($('btn-cancel-notes')) $('btn-cancel-notes').onclick = () => { editingNotes = false; renderAnalysis(); };
  if($('notes-form')) $('notes-form').onsubmit = (e) => {
    e.preventDefault();
    saveAnalysisNotes($('notes-textarea').value.trim());
  };

  // Recorte da amostra selecionada e botão de foco
  if (selected) {
    renderSampleCrop(selected);
    if ($('btn-focus-sample')) {
      $('btn-focus-sample').onclick = () => centerOnRegion(selected);
    }
  }

  // Disparar preparação assíncrona das camadas de segmentação
  prepareSegmentationCanvases();
}

async function exportClassifiedImage(){
  if(!current || !currentAnalysis) return;
  const btn = $('btn-export-classified-img');
  if(btn) btn.disabled = true;
  toast('Processando exportação da imagem classificada em alta resolução...');

  try {
    const img = new Image();
    img.crossOrigin = 'anonymous';
    await new Promise((resolve, reject) => {
      img.onload = resolve;
      img.onerror = () => reject(new Error('Falha ao obter os dados visuais da imagem'));
      img.src = `/api/v1/images/${encodeURIComponent(current.id)}/original-view`;
    });

    const w = img.naturalWidth || current.width || 1200;
    const h = img.naturalHeight || current.height || 1200;
    const scale = Math.max(1, Math.min(w, h) / 1200);
    const bannerH = Math.max(90, Math.round(75 * scale));

    const canvas = document.createElement('canvas');
    canvas.width = w;
    canvas.height = h + bannerH;
    const ctx = canvas.getContext('2d');

    // 1. Imagem base: usar a camada de sobreposição orgânica pixel-a-pixel (com filamentos reais destacados)
    if (segCanvases && segCanvases.overlay) {
      ctx.drawImage(segCanvases.overlay, 0, 0, w, h);
    } else {
      ctx.drawImage(img, 0, 0, w, h);
    }

    // 2. Traçado sutil das caixas de inspeção (apenas contorno fino, NUNCA preencher retângulos sólidos)
    const regions = currentAnalysis.regions || [];
    const lineWidth = Math.max(1, Math.round(1.0 * scale));

    for (const r of regions) {
      const finalLabel = r.review?.label || r.suggestion;
      let strokeColor = 'rgba(92,139,166,0.55)';
      if (finalLabel === 'ROOT') {
        strokeColor = 'rgba(32,182,138,0.75)';
      } else if (finalLabel === 'MATRIX') {
        strokeColor = 'rgba(230,168,23,0.65)';
      } else if (finalLabel === 'ARTIFACT') {
        strokeColor = 'rgba(224,86,56,0.65)';
      }

      ctx.lineWidth = lineWidth;
      ctx.strokeStyle = strokeColor;
      ctx.strokeRect(r.x, r.y, r.width, r.height);
    }

    // 3. Legenda científica discreta no canto superior direito
    const stats = currentAnalysis.statistics || {};
    const densPct = (w * h) > 0 && stats.total_root_area ? ((stats.total_root_area / (w * h)) * 100).toFixed(3) : '0.000';

    const legendItems = [
      { label: `Raízes (ROOT): ${stats.root_count || 0}`, color: '#20b68a' }
    ];
    if (stats.artifact_count > 0) {
      legendItems.push({ label: `Artefatos: ${stats.artifact_count}`, color: '#e05638' });
    }
    if (stats.matrix_count > 0) {
      legendItems.push({ label: `Solo / Matriz: ${stats.matrix_count}`, color: '#e6a817' });
    }
    if (stats.unknown_count > 0) {
      legendItems.push({ label: `Não confirmadas: ${stats.unknown_count}`, color: '#5c8ba6' });
    }

    const legFontSize = Math.max(11, Math.round(12 * scale));
    ctx.font = `600 ${legFontSize}px sans-serif`;
    let maxItemW = 0;
    for (const item of legendItems) {
      maxItemW = Math.max(maxItemW, ctx.measureText(item.label).width);
    }

    const legPad = 12 * scale;
    const legRowH = legFontSize + (8 * scale);
    const legW = maxItemW + (36 * scale) + (legPad * 2);
    const legH = (legendItems.length * legRowH) + (26 * scale);
    const legX = w - legW - (16 * scale);
    const legY = 16 * scale;

    // Fundo da legenda
    ctx.fillStyle = 'rgba(7, 30, 56, 0.85)';
    ctx.beginPath();
    const radius = 8 * scale;
    ctx.roundRect ? ctx.roundRect(legX, legY, legW, legH, radius) : ctx.rect(legX, legY, legW, legH);
    ctx.fill();
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.2)';
    ctx.lineWidth = 1 * scale;
    ctx.stroke();

    // Título da Legenda
    ctx.textBaseline = 'top';
    ctx.fillStyle = '#8cd7ed';
    ctx.font = `bold ${Math.max(10, Math.round(11 * scale))}px sans-serif`;
    ctx.fillText('LEGENDA CLASSIFICATÓRIA', legX + legPad, legY + (8 * scale));

    // Itens da Legenda
    let curY = legY + (24 * scale);
    for (const item of legendItems) {
      // Amostra de cor
      ctx.fillStyle = item.color;
      ctx.fillRect(legX + legPad, curY + (2 * scale), 12 * scale, 12 * scale);
      ctx.strokeStyle = '#ffffff';
      ctx.lineWidth = 0.8 * scale;
      ctx.strokeRect(legX + legPad, curY + (2 * scale), 12 * scale, 12 * scale);

      // Texto do item
      ctx.fillStyle = '#ffffff';
      ctx.font = `600 ${legFontSize}px sans-serif`;
      ctx.fillText(item.label, legX + legPad + (18 * scale), curY);
      curY += legRowH;
    }

    // 4. Rodapé científico com sumário estatístico
    ctx.fillStyle = '#071e38';
    ctx.fillRect(0, h, w, bannerH);

    ctx.strokeStyle = '#20b68a';
    ctx.lineWidth = Math.max(2, Math.round(3 * scale));
    ctx.beginPath();
    ctx.moveTo(0, h);
    ctx.lineTo(w, h);
    ctx.stroke();

    const titleSize = Math.max(13, Math.round(16 * scale));
    const subSize = Math.max(10, Math.round(12 * scale));

    ctx.fillStyle = '#45c8f5';
    ctx.font = `bold ${titleSize}px sans-serif`;
    ctx.fillText('MORFO · Análise Radicular Computacional', 20 * scale, h + (12 * scale));

    ctx.fillStyle = '#eaf2f8';
    ctx.font = `${subSize}px sans-serif`;
    const meta1 = `Amostra: ${current.sample_id || 'N/A'} | Arquivo: ${current.filename} | Resolução: ${w}×${h} px | SHA-256: ${(current.sha256||'').slice(0, 16)}…`;
    ctx.fillText(meta1, 20 * scale, h + (12 * scale) + titleSize + (5 * scale));

    ctx.fillStyle = '#2de4ad';
    ctx.font = `bold ${subSize}px sans-serif`;
    const meta2 = `Raízes: ${stats.root_count || 0} (${number.format(stats.total_root_area || 0)} px² · ${densPct}% da área total) | Solo: ${stats.matrix_count || 0} | Artefatos: ${stats.artifact_count || 0} | Status: Concluído (${stats.reviewed_count || 0}/${stats.total_regions || regions.length})`;
    ctx.fillText(meta2, 20 * scale, h + (12 * scale) + titleSize + subSize + (10 * scale));

    canvas.toBlob((blob) => {
      if(!blob){
        toast('Erro ao converter imagem classificada.');
        if(btn) btn.disabled = false;
        return;
      }
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = `rizoma-classificada-${current.sample_id || current.id}.png`;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      URL.revokeObjectURL(url);
      toast('Imagem classificada exportada com sucesso!');
      if(btn) btn.disabled = false;
    }, 'image/png');

  } catch(err) {
    toast('Erro na exportação: ' + err.message);
    if(btn) btn.disabled = false;
  }
}

async function setAnalysisStatus(newStatus){
  if(!currentAnalysis) return;
  try{
    const res = await api(`/api/v1/analysis-runs/${currentAnalysis.id}`, {
      method: 'PATCH',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({status: newStatus})
    });
    currentAnalysis.status = res.data.status;
    currentAnalysis.completed_at = res.data.completed_at;
    const item = analysisRuns.find(r => r.id === currentAnalysis.id);
    if(item) item.status = res.data.status;
    renderAnalysis();
    toast(newStatus === 'COMPLETED' ? 'Análise marcada como concluída!' : 'Análise arquivada.');
  }catch(err){ toast(err.message); }
}

async function saveAnalysisNotes(notesText){
  if(!currentAnalysis) return;
  try{
    const res = await api(`/api/v1/analysis-runs/${currentAnalysis.id}`, {
      method: 'PATCH',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({notes: notesText})
    });
    currentAnalysis.notes = res.data.notes;
    editingNotes = false;
    renderAnalysis();
    toast('Notas da análise salvas com sucesso.');
  }catch(err){ toast(err.message); }
}

function openDeleteAnalysisModal(){
  if(!currentAnalysis) return;
  $('delete-analysis-id').textContent = currentAnalysis.id.slice(0, 12) + '…';
  $('analysis-delete-modal').classList.remove('hidden');
}

function closeDeleteAnalysisModal(){
  $('analysis-delete-modal').classList.add('hidden');
}

async function confirmDeleteAnalysis(){
  if(!currentAnalysis) return;
  const deletedId = currentAnalysis.id;
  try{
    await api(`/api/v1/analysis-runs/${deletedId}`, {method: 'DELETE'});
    closeDeleteAnalysisModal();
    analysisRuns = analysisRuns.filter(r => r.id !== deletedId);
    if(analysisRuns.length > 0){
      await loadAnalysisRun(analysisRuns[0].id);
    } else {
      currentAnalysis = null;
      selectedRegion = null;
      renderAnalysis();
      fillInspect();
    }
    toast('Execução de análise excluída.');
  }catch(err){ toast(err.message); }
}

function openBatchConfirmModal(){
  if(!currentAnalysis) return;
  const pending = (currentAnalysis.regions || []).filter(r => !r.review);
  if(!pending.length){ toast('Todas as regiões já foram revisadas!'); return; }
  $('batch-confirm-count').textContent = pending.length;
  $('batch-confirm-modal').classList.remove('hidden');
}

function closeBatchConfirmModal(){
  $('batch-confirm-modal').classList.add('hidden');
}

async function submitBatchConfirm(e){
  if(e && e.preventDefault) e.preventDefault();
  if(!currentAnalysis) return;
  const justification = $('batch-confirm-justification').value.trim();
  const btn = $('btn-submit-batch-confirm');
  btn.disabled = true; btn.textContent = 'Confirmando…';
  try{
    const res = await api(`/api/v1/analysis-runs/${currentAnalysis.id}/confirm-all`, {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({justification})
    });
    closeBatchConfirmModal();
    toast(`${res.confirmed_count} região(ões) confirmada(s) em lote com sucesso!`);
    await loadAnalysisRun(currentAnalysis.id);
  }catch(err){ toast(err.message); }
  finally{ btn.disabled = false; btn.textContent = 'Confirmar todas em lote ⚡'; }
}

function reviewEditor(region){
  const currentHypothesis = region.review?.label || region.suggestion;
  const isUnknown = currentHypothesis === 'UNKNOWN';
  const label = region.review?.label || (isUnknown ? 'ROOT' : region.suggestion);
  const jev = region.judgment_evidence || {};
  const learning = jev.learning || {};
  const conf = jev.confidence ? Math.round(jev.confidence * 100) : null;
  const strat = learning.strategy || 'BASELINE';
  const isRecalibrated = !region.review && jev.recalibrated === true;
  const badge = conf !== null ? `<div class="learning-badge ${learning.uncertainty ? 'uncertain' : ''} ${isRecalibrated ? 'recalibrated' : ''}">Estratégia: ${h(strat)} · Confiança estimada: ${conf}% ${isRecalibrated ? '⚡ Recalibrada em tempo real' : ''} ${learning.uncertainty ? '⚠️ Alta Incerteza (Prioridade de Revisão)' : ''}</div>` : '';
  const nextReg = findNextRecommendedRegion(region.id);
  const nextBtn = nextReg ? `<button type="button" id="btn-next-review" class="button outline full" style="margin-top:6px">Salvar e ir para próxima recomendada (R${nextReg.ordinal}) ➜</button>` : '';

  const sampleCropHtml = `
    <div class="sample-crop-wrap">
      <div class="sample-crop-head">
        <span>AMOSTRA SELECIONADA</span>
        <strong>R${region.ordinal} · ${region.suggestion === 'ROOT' ? '🌿 Possível Raiz' : '🟤 Solo'}</strong>
      </div>
      <canvas id="sample-crop-canvas" class="sample-crop-canvas" width="280" height="160" title="Recorte ampliado da amostra R${region.ordinal}"></canvas>
      <div class="sample-crop-foot">
        <span>${number.format(region.width)} × ${number.format(region.height)} px (${number.format(region.area)} px²)</span>
        <button type="button" class="btn-focus-sample" id="btn-focus-sample" title="Centralizar visualização nesta amostra">🔍 Focar no monolito</button>
      </div>
    </div>
  `;

  const actionDefault = isUnknown ? 'CORRECT' : (region.review ? 'CORRECT' : 'CONFIRM');

  return `<form id="review-form" class="review-form">
    <h3>Revisar região R${region.ordinal}</h3>
    ${sampleCropHtml}
    ${badge}
    <p class="automatic-hypothesis">Hipótese automática: <strong>${h(automaticLabel(region.suggestion, isRecalibrated))}</strong>. Concordância é evidência técnica, não confiança científica.</p>
    <div class="evidence"><span>Global: ${number.format(region.evidence.global_pixels)}</span><span>Local: ${number.format(region.evidence.local_pixels)}</span><span>Concordância: ${number.format(region.evidence.shared_pixels)} px</span></div>
    <label>Ação<select id="review-action"><option value="CONFIRM" ${actionDefault==='CONFIRM'?'selected':''}>Confirmar interpretação</option><option value="CORRECT" ${actionDefault==='CORRECT'?'selected':''}>${isUnknown ? 'Classificar / Atribuir rótulo' : 'Corrigir classificação'}</option><option value="SUSPEND">Suspender julgamento</option></select></label>
    <label>Classificação<select id="review-label">${['ROOT','MATRIX','ARTIFACT','UNKNOWN'].map(v => `<option ${v===label?'selected':''}>${v}</option>`).join('')}</select></label>
    <label>Justificativa<textarea id="review-justification" maxlength="1000" placeholder="Evidência ou motivo da decisão">${h(region.review?.justification || '')}</textarea></label>
    <button class="button primary full" type="submit">Salvar nova revisão</button>
    ${nextBtn}
    <div id="review-history" class="review-history"><small>Carregando histórico…</small></div>
  </form>`;
}

async function loadHistory(regionId){
  try{
    const result = await api(`/api/v1/analysis-regions/${regionId}/reviews`);
    if(!selectedRegion || selectedRegion.id !== regionId || !$('review-history')) return;
    $('review-history').innerHTML = result.data.length ? `<h4>Histórico preservado</h4>${result.data.map(e => `<div><strong>v${e.version} · ${h(e.action)} → ${h(e.label)}</strong><small>${h(e.created_at)} · ${h(e.reviewer)}</small><p>${h(e.justification || 'Sem justificativa')}</p></div>`).join('')}` : '<small>Nenhuma revisão humana registrada.</small>';
  }catch(err){ if($('review-history')) $('review-history').textContent = err.message; }
}

async function startAnalysis(){
  if(!current) return;
  const button = $('start-analysis');
  const emptyBtn = $('btn-start-analysis-empty');
  if(button){ button.disabled = true; button.textContent = 'Analisando…'; }
  if(emptyBtn){ emptyBtn.disabled = true; emptyBtn.textContent = 'Analisando e treinando modelo…'; }
  const strategy = $('analysis-strategy')?.value || $('analysis-strategy-empty')?.value || 'BAYESIAN_CALIBRATED';
  try{
    const result = await api(`/api/v1/images/${current.id}/analysis?algorithm=${encodeURIComponent(strategy)}`, {method:'POST'});
    currentAnalysis = result.data;
    const runs = await api(`/api/v1/images/${current.id}/analysis-runs`);
    analysisRuns = runs.data || [];
    selectedRegion = currentAnalysis.regions?.[0] || null;
    renderAnalysis();
    if(selectedRegion) {
      loadHistory(selectedRegion.id);
      centerOnRegion(selectedRegion);
      renderSampleCrop(selectedRegion);
    }
    fillInspect();
    toast(`${currentAnalysis.regions.length} região(ões) identificadas e modelo treinado via ${strategy}.`);
  }catch(err){ toast(err.message); }
  finally{
    if(button){ button.disabled = false; button.textContent = '↻ Re-analisar'; }
  }
}

async function saveReview(event, advanceNext = false){
  if(event && event.preventDefault) event.preventDefault();
  if(!selectedRegion) return;
  const targetRegionId = selectedRegion.id;
  const activeRunId = currentAnalysis?.id;
  let action = $('review-action')?.value || 'CONFIRM';
  let label = action === 'SUSPEND' ? 'UNKNOWN' : ($('review-label')?.value || 'ROOT');
  const curHypo = selectedRegion.review?.label || selectedRegion.suggestion;

  // Se o usuário selecionou CONFIRM mas o rótulo é diferente da hipótese atual,
  // ou se a hipótese é UNKNOWN e o usuário escolheu ROOT/MATRIX/ARTIFACT, ajusta para CORRECT
  if(action === 'CONFIRM' && label !== curHypo){
    action = 'CORRECT';
  }

  const nextTarget = advanceNext ? findNextRecommendedRegion(targetRegionId) : null;
  try{
    const revRes = await api(`/api/v1/analysis-regions/${targetRegionId}/reviews`, {
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({action,label,justification:$('review-justification').value.trim(),expected_version:selectedRegion.version})
    });
    const url = `/api/v1/images/${current.id}/analysis` + (activeRunId ? `?run_id=${encodeURIComponent(activeRunId)}` : '');
    const result = await api(url);
    currentAnalysis = result.data;
    selectedRegion = nextTarget ? regionById(nextTarget.id) : regionById(targetRegionId);
    renderAnalysis();
    if(selectedRegion) loadHistory(selectedRegion.id);

    // Retreinar o modelo estatístico e atualizar todas as camadas científicas em tempo real!
    await prepareSegmentationCanvases(true);

    const recalibrated = revRes?.recalibrated_count || 0;
    if(recalibrated > 0){
      toast(`Revisão salva! Modelo retreinado e ${recalibrated} região(ões) recalibradas em tempo real.`);
    } else {
      toast('Revisão salva e classificação atualizada em tempo real!');
    }
  }catch(err){
    toast(err.message === 'Review conflict' ? 'Conflito: a região foi revisada em outra sessão. Recarregue a análise.' : err.message);
  }
}

function closeInspect(){
  window.dispatchEvent(new CustomEvent('morfo:image-closed'));
  $('inspector').classList.add('hidden');
  closeEditImageModal();
  closeDeleteImageModal();
  closeDeleteAnalysisModal();
  closeBatchConfirmModal();
  if($('auth-overlay').classList.contains('hidden')){
    document.body.style.overflow = '';
  }
  current = null;
  currentAnalysis = null;
  analysisRuns = [];
  selectedRegion = null;
  editingNotes = false;
  viewer.revision++; viewer.imageId = null; viewer.manifest = null; viewer.drag = null;
  cancelAnimationFrame(viewer.frame); $('tile-layer').replaceChildren(); $('inspect-image').removeAttribute('src');
}

function openEditImageModal(){
  if(!current) return;
  $('edit-sample-id').value = current.sample_id || '';
  $('edit-capture-date').value = current.capture_date || '';
  $('edit-filename').value = current.filename || '';
  $('image-edit-modal').classList.remove('hidden');
  $('edit-filename').focus();
}

function closeEditImageModal(){
  $('image-edit-modal').classList.add('hidden');
}

async function saveEditImage(e){
  e.preventDefault();
  if(!current) return;
  const sample_id = $('edit-sample-id').value.trim();
  const capture_date = $('edit-capture-date').value;
  const filename = $('edit-filename').value.trim();
  if(!filename){ toast('Nome do arquivo é obrigatório'); return; }
  try{
    const res = await api(`/api/v1/images/${current.id}`, {
      method: 'PATCH',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({sample_id, capture_date, filename})
    });
    current.sample_id = res.data.sample_id;
    current.capture_date = res.data.capture_date;
    current.filename = res.data.filename;
    const inList = images.find(img => img.id === current.id);
    if(inList){
      inList.sample_id = res.data.sample_id;
      inList.capture_date = res.data.capture_date;
      inList.filename = res.data.filename;
    }
    $('inspect-title').textContent = current.filename;
    fillInspect();
    render();
    closeEditImageModal();
    toast('Metadados da imagem atualizados.');
  }catch(err){ toast(err.message); }
}

function openDeleteImageModal(){
  if(!current) return;
  $('delete-image-name').textContent = current.filename;
  $('image-delete-modal').classList.remove('hidden');
}

function closeDeleteImageModal(){
  $('image-delete-modal').classList.add('hidden');
}

async function confirmDeleteImage(){
  if(!current) return;
  const imageId = current.id;
  try{
    await api(`/api/v1/images/${imageId}`, {method: 'DELETE'});
    closeDeleteImageModal();
    closeInspect();
    images = images.filter(img => img.id !== imageId);
    render();
    toast('Imagem removida do acervo.');
  }catch(err){ toast(err.message); }
}

function filesSelected(files){
  selectedFiles = Array.from(files);
  $('file-count').textContent = selectedFiles.length ? `${selectedFiles.length} arquivo(s) selecionado(s)` : 'Nenhum arquivo selecionado';
  $('file-list').innerHTML = selectedFiles.map(f => `<li>${h(f.name)} — ${(f.size / 1024 / 1024).toFixed(2)} MiB</li>`).join('');
  $('send').disabled = selectedFiles.length === 0;
}

async function upload(e){
  e.preventDefault();
  if(!selectedFiles.length) return;
  const btn = $('send');
  btn.disabled = true;
  const sample = $('sample-id').value.trim(), date = $('capture-date').value;
  const results = $('upload-results');
  results.innerHTML = '';
  let successes = 0;
  for(let i = 0; i < selectedFiles.length; i++){
    const file = selectedFiles[i];
    const status = document.createElement('p');
    status.textContent = `Enviando ${i + 1}/${selectedFiles.length}: ${file.name}…`;
    results.append(status);
    try{
      if(file.size > 8 * 1024 * 1024 * 1024) throw Error('Arquivo acima do limite operacional de 8 GiB');
      const url = new URL('/api/v1/images', location.href);
      url.searchParams.set('filename', file.name);
      url.searchParams.set('sample_id', sample);
      url.searchParams.set('capture_date', date);
      await api(url.pathname + url.search, {
        method: 'POST',
        headers: {'Content-Type': 'application/octet-stream', 'X-Morfo-Client': 'web-v0.2.0'},
        body: file
      });
      status.textContent = `✓ ${file.name}: original preservado e prévias geradas.`;
      successes++;
    }catch(err){
      status.classList.add('fail');
      status.textContent = `✕ ${file.name}: ${err.message}`;
    }
  }
  btn.disabled = false;
  toast(`${successes} de ${selectedFiles.length} arquivo(s) registrado(s).`);
  await refresh();
  if(successes === selectedFiles.length){
    $('upload-form').reset();
    filesSelected([]);
  }
}

// Resizable scientific workspace. Size is local UI preference, never scientific data.
const inspectorModal = document.querySelector('#inspector > .modal');
const inspectorResizeHandle = $('inspector-resize-handle');
const inspectorSizeKey = 'morfo.inspector.size.v1';
function inspectorLimits(){
  return {minWidth:Math.min(900,window.innerWidth-14),minHeight:Math.min(600,window.innerHeight-14),maxWidth:window.innerWidth-14,maxHeight:window.innerHeight-14};
}
function setInspectorSize(width,height,persist=true){
  if(window.innerWidth<=900)return;
  const limits=inspectorLimits();
  const w=Math.max(limits.minWidth,Math.min(limits.maxWidth,Math.round(width)));
  const h=Math.max(limits.minHeight,Math.min(limits.maxHeight,Math.round(height)));
  inspectorModal.style.width=`${w}px`;inspectorModal.style.height=`${h}px`;
  if(persist)try{localStorage.setItem(inspectorSizeKey,JSON.stringify({width:w,height:h}));}catch(_error){}
}
function restoreInspectorSize(){
  if(window.innerWidth<=900)return;
  try{const saved=JSON.parse(localStorage.getItem(inspectorSizeKey)||'null');if(saved&&Number.isFinite(saved.width)&&Number.isFinite(saved.height))setInspectorSize(saved.width,saved.height,false);}catch(_error){}
}
function resetInspectorSize(){
  try{localStorage.removeItem(inspectorSizeKey);}catch(_error){}
  inspectorModal.style.removeProperty('width');inspectorModal.style.removeProperty('height');
}
let inspectorResize=null;
inspectorResizeHandle.addEventListener('pointerdown',event=>{
  if(window.innerWidth<=900)return;const bounds=inspectorModal.getBoundingClientRect();
  inspectorResize={pointer:event.pointerId,x:event.clientX,y:event.clientY,width:bounds.width,height:bounds.height};
  inspectorResizeHandle.setPointerCapture(event.pointerId);inspectorResizeHandle.classList.add('dragging');document.body.classList.add('resizing-inspector');event.preventDefault();
});
inspectorResizeHandle.addEventListener('pointermove',event=>{if(!inspectorResize||event.pointerId!==inspectorResize.pointer)return;setInspectorSize(inspectorResize.width+event.clientX-inspectorResize.x,inspectorResize.height+event.clientY-inspectorResize.y);});
function stopInspectorResize(event){if(!inspectorResize||event.pointerId!==inspectorResize.pointer)return;inspectorResize=null;inspectorResizeHandle.classList.remove('dragging');document.body.classList.remove('resizing-inspector');}
inspectorResizeHandle.addEventListener('pointerup',stopInspectorResize);inspectorResizeHandle.addEventListener('pointercancel',stopInspectorResize);
inspectorResizeHandle.addEventListener('dblclick',()=>setInspectorSize(window.innerWidth-14,window.innerHeight-14));
inspectorResizeHandle.addEventListener('keydown',event=>{if(!['ArrowLeft','ArrowRight','ArrowUp','ArrowDown'].includes(event.key))return;const bounds=inspectorModal.getBoundingClientRect(),step=event.shiftKey?100:30;setInspectorSize(bounds.width+(event.key==='ArrowRight'?step:event.key==='ArrowLeft'?-step:0),bounds.height+(event.key==='ArrowDown'?step:event.key==='ArrowUp'?-step:0));event.preventDefault();});
$('reset-inspector-size').addEventListener('click',resetInspectorSize);
restoreInspectorSize();

for(const b of document.querySelectorAll('[data-view]')) b.addEventListener('click', () => nav(b.dataset.view));
$('hero-upload').onclick = () => nav('recebimento');
$('refresh').onclick = refresh;
$('refresh-root-analyses').onclick = loadRootMetricRecords;
$('root-analysis-search').oninput = renderRootMetricRecords;
$('root-analysis-filter').onchange = renderRootMetricRecords;
$('search').oninput = render;
$('filter').onchange = render;
$('upload-form').addEventListener('submit', upload);

$('file-input').onchange = e => filesSelected(e.target.files);
const drop = $('dropzone');
for(const event of ['dragenter', 'dragover']) drop.addEventListener(event, e => { e.preventDefault(); drop.classList.add('drag'); });
for(const event of ['dragleave', 'drop']) drop.addEventListener(event, e => { e.preventDefault(); drop.classList.remove('drag'); });
drop.addEventListener('drop', e => filesSelected(e.dataTransfer.files));

$('close-inspect').onclick = closeInspect;
$('inspector').addEventListener('click', e => { if(e.target === $('inspector')) closeInspect(); });
document.addEventListener('keydown', e => { if(e.key === 'Escape' && !$('inspector').classList.contains('hidden')) closeInspect(); });

for(const b of document.querySelectorAll('[data-image-mode]')) b.onclick = () => { mode = b.dataset.imageMode; fillInspect(); };
$('zoom').oninput = e => setViewerZoom(Number(e.target.value) / 100);
$('zoom-in').onclick = () => setViewerZoom(viewer.scale * 1.35);
$('zoom-out').onclick = () => setViewerZoom(viewer.scale / 1.35);
$('zoom-fit').onclick = fitViewer;
$('zoom-actual').onclick = () => setViewerZoom(1);
$('start-analysis').onclick = startAnalysis;

const imageStage = $('image-stage');
imageStage.addEventListener('wheel', e => {
  if(!current) return;
  e.preventDefault();
  const bounds = imageStage.getBoundingClientRect();
  setViewerZoom(viewer.scale * Math.exp(-e.deltaY * .0015), e.clientX - bounds.left, e.clientY - bounds.top);
}, {passive:false});
imageStage.addEventListener('pointerdown', e => {
  if(!current || e.button !== 0 || e.target.closest('.region-box')) return;
  viewer.drag = {pointer:e.pointerId, clientX:e.clientX, clientY:e.clientY, x:viewer.x, y:viewer.y};
  imageStage.setPointerCapture(e.pointerId); imageStage.classList.add('panning');
});
imageStage.addEventListener('pointermove', e => {
  const bounds = imageStage.getBoundingClientRect();
  const imageX = Math.floor((e.clientX - bounds.left - viewer.x) / viewer.scale), imageY = Math.floor((e.clientY - bounds.top - viewer.y) / viewer.scale);
  $('cursor-position').textContent = current && imageX >= 0 && imageY >= 0 && imageX < current.width && imageY < current.height ? `x ${number.format(imageX)} · y ${number.format(imageY)}` : '—';
  if(!viewer.drag || viewer.drag.pointer !== e.pointerId) return;
  viewer.x = viewer.drag.x + e.clientX - viewer.drag.clientX; viewer.y = viewer.drag.y + e.clientY - viewer.drag.clientY;
  viewer.fit = false; renderViewer();
});
function stopViewerDrag(e){
  if(!viewer.drag || viewer.drag.pointer !== e.pointerId) return;
  viewer.drag = null; imageStage.classList.remove('panning');
}
imageStage.addEventListener('pointerup', stopViewerDrag);
imageStage.addEventListener('pointercancel', stopViewerDrag);
imageStage.addEventListener('pointerleave', () => { if(!viewer.drag) $('cursor-position').textContent = '—'; });
imageStage.addEventListener('dblclick', e => {
  const bounds = imageStage.getBoundingClientRect(); setViewerZoom(viewer.scale * 2, e.clientX - bounds.left, e.clientY - bounds.top);
});
imageStage.addEventListener('keydown', e => {
  if(!current) return;
  if(e.key === '+' || e.key === '=') setViewerZoom(viewer.scale * 1.25);
  else if(e.key === '-') setViewerZoom(viewer.scale / 1.25);
  else if(e.key === '0') fitViewer();
  else if(e.key === '1') setViewerZoom(1);
  else if(['ArrowLeft','ArrowRight','ArrowUp','ArrowDown'].includes(e.key)){
    const delta=60;if(e.key==='ArrowLeft')viewer.x+=delta;if(e.key==='ArrowRight')viewer.x-=delta;if(e.key==='ArrowUp')viewer.y+=delta;if(e.key==='ArrowDown')viewer.y-=delta;viewer.fit=false;renderViewer();
  } else return;
  e.preventDefault();
});
window.addEventListener('resize', () => {
  if(window.innerWidth>900&&inspectorModal.style.width)setInspectorSize(parseFloat(inspectorModal.style.width),parseFloat(inspectorModal.style.height),false);
  if(!current)return;if(viewer.fit)fitViewer();else renderViewer();
});

$('menu').onclick = () => {
  const open = $('sidebar').classList.toggle('open');
  $('mobile-scrim').classList.toggle('shown', open);
  $('menu').setAttribute('aria-expanded', String(open));
};
$('mobile-scrim').onclick = () => {
  $('sidebar').classList.remove('open');
  $('mobile-scrim').classList.remove('shown');
};

// Auth form listeners
$('auth-email-form').addEventListener('submit', requestAuthCode);
$('auth-code-form').addEventListener('submit', verifyAuthCode);
$('btn-resend-code').addEventListener('click', requestAuthCode);
$('btn-switch-email').addEventListener('click', () => {
  $('auth-code-form').classList.add('hidden');
  $('auth-email-form').classList.remove('hidden');
  setAuthFeedback('');
  $('auth-email-input').focus();
});
$('btn-logout').addEventListener('click', logout);

// Initialize view and check session
const initial = location.hash.slice(1);
nav(viewNames[initial] ? initial : 'acervo');
refresh();

// Image and Analysis CRUD modal listeners
$('btn-edit-image').onclick = openEditImageModal;
$('close-edit-image').onclick = closeEditImageModal;
$('btn-cancel-edit-image').onclick = closeEditImageModal;
$('edit-image-form').onsubmit = saveEditImage;

$('btn-delete-image').onclick = openDeleteImageModal;
$('close-delete-image').onclick = closeDeleteImageModal;
$('btn-cancel-delete-image').onclick = closeDeleteImageModal;
$('btn-confirm-delete-image').onclick = confirmDeleteImage;

$('close-delete-analysis').onclick = closeDeleteAnalysisModal;
$('btn-cancel-delete-analysis').onclick = closeDeleteAnalysisModal;
$('btn-confirm-delete-analysis').onclick = confirmDeleteAnalysis;

$('close-batch-confirm').onclick = closeBatchConfirmModal;
$('btn-cancel-batch-confirm').onclick = closeBatchConfirmModal;
$('batch-confirm-form').onsubmit = submitBatchConfirm;

// Previne o zoom acidental da pagina inteira pelo navegador via Ctrl + Roda do mouse
window.addEventListener('wheel', e => { if(e.ctrlKey) e.preventDefault(); }, { passive: false });


// MORFO bridge is read-only; no exposure of credentials or private file paths.
window.MORFO_Bridge = { activeImageId: () => current?.id ?? null, activeImageMeta: () => current ? { id:current.id, width:current.width, height:current.height, sha256:current.sha256 } : null };

// MORFO-UX-002: legacy classifiers retained in code for historical regression, hidden from thematic workspace.

// MORFO-DEV-ROI-001: regional mask refinement. Coordinates always use original raster.
const refinement={id:null,state:null,tool:'rectangle',points:[],dragStart:null,previewUrl:null,request:0,drawing:false,presets:[],defaults:null};
const refinementParameterFields={score_threshold:'rp-score',brightness_threshold:'rp-brightness',min_component_area:'rp-min-area',max_component_area:'rp-max-area',min_elongation:'rp-elongation',fill_factor:'rp-fill',top_hat_weight:'rp-weight',small_component_limit:'rp-small'};
const refinementParameterLabels={score_threshold:'pontuação',brightness_threshold:'brilho',min_component_area:'área mínima',max_component_area:'área máxima',min_elongation:'alongamento',fill_factor:'preenchimento',top_hat_weight:'peso top-hat',small_component_limit:'limite pequeno'};
function readRefinementParameters(){const values={top_hat_width:17,top_hat_height:17,median_width:3,median_height:3,opening_width:3,opening_height:3};for(const [key,id] of Object.entries(refinementParameterFields))values[key]=Number($(id).value);return values;}
function writeRefinementParameters(values){for(const [key,id] of Object.entries(refinementParameterFields))if(values?.[key]!=null)$(id).value=values[key];compareRefinementParameters();}
function selectedRefinementPreset(){return refinement.presets.find(item=>item.id===$('refinement-preset').value)||null;}
function compareRefinementParameters(){const target=selectedRefinementPreset()?.parameters||refinement.defaults;if(!target)return;const current=readRefinementParameters(),changed=Object.keys(refinementParameterFields).filter(key=>Number(current[key])!==Number(target[key]));$('refinement-preset-comparison').textContent=changed.length?`${changed.length} diferença(s) frente a ${selectedRefinementPreset()?.name||'referência'}: ${changed.map(key=>refinementParameterLabels[key]).join(', ')}.`:`Parâmetros iguais a ${selectedRefinementPreset()?.name||'valores de referência'}.`;}
function renderRefinementPresets(selected=''){const picker=$('refinement-preset');picker.replaceChildren(new Option('Valores de referência',''));for(const item of refinement.presets)picker.append(new Option(`${item.name} · v${item.version}`,item.id));picker.value=selected;const preset=selectedRefinementPreset();$('refinement-preset-name').value=preset?.name||'';$('refinement-preset-delete').disabled=!preset;compareRefinementParameters();}
async function loadRefinementPresets(){const [contract,saved]=await Promise.all([api('/api/v1/refinement-parameters'),api('/api/v1/parameter-presets')]);refinement.defaults=contract.data.defaults;refinement.presets=saved.data||[];renderRefinementPresets();}
function loadSelectedRefinementPreset(){const preset=selectedRefinementPreset();writeRefinementParameters(preset?.parameters||refinement.defaults);$('refinement-preset-name').value=preset?.name||'';$('refinement-preset-delete').disabled=!preset;toast(preset?`Configuração “${preset.name}” carregada.`:'Valores de referência restaurados.');}
async function saveRefinementPreset(){const name=$('refinement-preset-name').value.trim();if(!name){toast('Informe nome para configuração.');return;}try{const saved=(await api('/api/v1/parameter-presets',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name,...readRefinementParameters()})})).data;refinement.presets=(await api('/api/v1/parameter-presets')).data||[];renderRefinementPresets(saved.id);toast('Configuração salva.');}catch(error){toast(error.message);}}
async function deleteRefinementPreset(){const preset=selectedRefinementPreset();if(!preset||!confirm(`Excluir configuração “${preset.name}”?`))return;try{await api(`/api/v1/parameter-presets/${preset.id}`,{method:'DELETE'});refinement.presets=refinement.presets.filter(item=>item.id!==preset.id);renderRefinementPresets();toast('Configuração excluída.');}catch(error){toast(error.message);}}
function refinementPayload(){
 const action=$('refinement-action').value,p={intervention_type:action,selection_type:$('refinement-tool').value};
 if(p.selection_type==='rectangle'){if(refinement.points.length!==2)throw new Error('Desenhe um retângulo.');const [a,b]=refinement.points;p.x=Math.floor(Math.min(a[0],b[0]));p.y=Math.floor(Math.min(a[1],b[1]));p.width=Math.max(1,Math.ceil(Math.abs(a[0]-b[0])));p.height=Math.max(1,Math.ceil(Math.abs(a[1]-b[1])));}
 else{if(refinement.points.length<(p.selection_type==='polygon'?3:1))throw new Error('Seleção incompleta.');p.points=refinement.points.flat();if(p.selection_type==='brush')p.brush_diameter=Number($('refinement-brush').value);}
 if(action==='REPROCESS')Object.assign(p,readRefinementParameters());
 return p;
}
function refinementPoint(event){const canvas=$('refinement-selection'),rect=canvas.getBoundingClientRect();return [(event.clientX-rect.left)*canvas.width/rect.width,(event.clientY-rect.top)*canvas.height/rect.height];}
function drawRefinementSelection(){const canvas=$('refinement-selection'),ctx=canvas.getContext('2d');ctx.clearRect(0,0,canvas.width,canvas.height);if(!refinement.points.length)return;ctx.strokeStyle='#ffcf48';ctx.fillStyle='#ffcf4838';ctx.lineWidth=Math.max(2,canvas.width/500);ctx.lineJoin='round';ctx.lineCap='round';const tool=$('refinement-tool').value;if(tool==='rectangle'&&refinement.points.length===2){const [a,b]=refinement.points;ctx.fillRect(a[0],a[1],b[0]-a[0],b[1]-a[1]);ctx.strokeRect(a[0],a[1],b[0]-a[0],b[1]-a[1]);return;}ctx.beginPath();ctx.moveTo(...refinement.points[0]);refinement.points.slice(1).forEach(point=>ctx.lineTo(...point));if(tool==='polygon'&&refinement.points.length>2){ctx.closePath();ctx.fill();}if(tool==='brush')ctx.lineWidth=Number($('refinement-brush').value);ctx.stroke();}
function clearRefinementSelection(){refinement.points=[];refinement.dragStart=null;drawRefinementSelection();}
function renderRefinementState(state){refinement.state=state;const current=state.current_revision_id;$('refinement-history').innerHTML=state.revisions.map(r=>`<li class="${r.id===current?'active':''}"><strong>${h(r.type)}</strong> · +${number.format(r.added_pixels)} / −${number.format(r.removed_pixels)}<br><small>${h(r.created_at)}</small></li>`).join('');$('refinement-undo').disabled=state.revisions[0]?.id===current||state.status!=='DRAFT';$('refinement-redo').disabled=!state.can_redo||state.status!=='DRAFT';$('refinement-apply').disabled=state.status!=='DRAFT';$('refinement-preview').disabled=state.status!=='DRAFT';$('refinement-confirm').disabled=state.status!=='DRAFT';$('refinement-status').textContent=state.status==='CONFIRMED'?'Resultado confirmado e congelado.':`${state.revisions.length-1} intervenção(ões) · rascunho não confirmado`;}
function refreshRefinementImage(){if(refinement.previewUrl){URL.revokeObjectURL(refinement.previewUrl);refinement.previewUrl=null;}if($('refinement-cancel-preview'))$('refinement-cancel-preview').disabled=true;const stamp=Date.now(),base=`/api/v1/refinements/${refinement.id}`;$('refinement-image').src=`${base}/overlay?t=${stamp}`;$('refinement-before').src=`${base}/before?t=${stamp}`;$('refinement-proposed').src=`${base}/mask?t=${stamp}`;$('refinement-difference').src=`${base}/difference?t=${stamp}`;$('refinement-export-package').href=`${base}/export.zip`;$('refinement-export-mask').href=`${base}/mask`;$('refinement-export-overlay').href=`${base}/overlay`;$('refinement-export-metadata').href=`${base}/metadata`;}
async function openRefinement(){if(!current)return;const button=$('start-refinement');button.disabled=true;try{let runs=(await api(`/api/v1/images/${current.id}/profiles/roots-exp003`)).data||[];let base=runs[0];if(!base)base=(await api(`/api/v1/images/${current.id}/profiles/roots-exp003`,{method:'POST'})).data;const sessions=(await api(`/api/v1/images/${current.id}/refinements`)).data||[],parent=sessions.find(item=>item.status==='CONFIRMED'&&item.base_profile_run_id===base.id&&item.original_sha256===current.sha256);const createBody={base_profile_run_id:base.id};if(parent)createBody.source_refinement_id=parent.id;const created=await api(`/api/v1/images/${current.id}/refinements`,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(createBody)});refinement.id=created.data.id;refinement.state=created.data;await loadRefinementPresets();writeRefinementParameters(refinement.defaults);clearRefinementSelection();$('refinement-modal').classList.remove('hidden');const img=$('refinement-image');img.onload=()=>{const canvas=$('refinement-selection');canvas.width=current.width;canvas.height=current.height;canvas.style.width=img.width+'px';canvas.style.height=img.height+'px';drawRefinementSelection();};img.style.opacity=Number($('refinement-opacity').value)/100;refreshRefinementImage();renderRefinementState((await api(`/api/v1/refinements/${refinement.id}`)).data);if(parent)$('refinement-status').textContent='Refinamento anterior carregado como base. Novas alterações serão acumuladas.';}catch(error){toast(error.message);}finally{button.disabled=false;}}
async function previewRefinement(){try{const sequence=++refinement.request,payload=refinementPayload();$('refinement-status').textContent='Calculando prévia…';$('refinement-cancel-preview').disabled=false;const response=await fetch(`/api/v1/refinements/${refinement.id}/preview`,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});if(!response.ok){const problem=await response.json();throw new Error(problem.error||'Falha na prévia');}const blob=await response.blob();if(sequence!==refinement.request)return;if(refinement.previewUrl)URL.revokeObjectURL(refinement.previewUrl);refinement.previewUrl=URL.createObjectURL(blob);$('refinement-image').src=refinement.previewUrl;$('refinement-proposed').src=refinement.previewUrl;$('refinement-status').textContent='Prévia experimental; ainda não aplicada. Use “Desfazer teste” para voltar ao resultado anterior.';}catch(error){$('refinement-cancel-preview').disabled=true;toast(error.message);}}
function cancelRefinementPreview(){++refinement.request;refreshRefinementImage();if(refinement.state)renderRefinementState(refinement.state);$('refinement-status').textContent='Teste desfeito. Resultado consolidado anterior restaurado.';}
async function applyRefinement(){try{const state=(await api(`/api/v1/refinements/${refinement.id}/interventions`,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(refinementPayload())})).data;renderRefinementState(state);clearRefinementSelection();refreshRefinementImage();}catch(error){toast(error.message);}}
async function navigateRefinement(action){try{renderRefinementState((await api(`/api/v1/refinements/${refinement.id}/${action}`,{method:'POST'})).data);refreshRefinementImage();}catch(error){toast(error.message);}}
const refinementCanvas=$('refinement-selection');refinementCanvas.onpointerdown=event=>{const tool=$('refinement-tool').value,point=refinementPoint(event);if(tool==='rectangle'){refinement.dragStart=point;refinement.points=[point,point];refinement.drawing=true;refinementCanvas.setPointerCapture(event.pointerId);}else if(tool==='brush'){refinement.points=[point];refinement.drawing=true;refinementCanvas.setPointerCapture(event.pointerId);}else{refinement.points.push(point);drawRefinementSelection();}};
refinementCanvas.onpointermove=event=>{if(!refinement.drawing)return;const point=refinementPoint(event);if($('refinement-tool').value==='rectangle')refinement.points[1]=point;else refinement.points.push(point);drawRefinementSelection();};refinementCanvas.onpointerup=()=>{refinement.drawing=false;};
$('start-refinement').onclick=openRefinement;$('close-refinement').onclick=()=>{$('refinement-modal').classList.add('hidden');++refinement.request;};$('refinement-clear').onclick=clearRefinementSelection;$('refinement-tool').onchange=clearRefinementSelection;$('refinement-preview').onclick=previewRefinement;$('refinement-cancel-preview').onclick=cancelRefinementPreview;$('refinement-apply').onclick=applyRefinement;$('refinement-undo').onclick=()=>navigateRefinement('undo');$('refinement-redo').onclick=()=>navigateRefinement('redo');$('refinement-confirm').onclick=async()=>{try{const confirmed=(await api(`/api/v1/refinements/${refinement.id}/confirm`,{method:'POST'})).data;renderRefinementState(confirmed);window.dispatchEvent(new CustomEvent('morfo:refinement-confirmed',{detail:{sessionId:refinement.id,state:confirmed}}));$('refinement-modal').classList.add('hidden');++refinement.request;toast('Resultado confirmado. Sobreposição consolidada exibida na análise.');}catch(error){toast(error.message);}};$('refinement-brush').oninput=()=>{$('refinement-brush-value').textContent=$('refinement-brush').value+' px';drawRefinementSelection();};$('refinement-opacity').oninput=()=>{$('refinement-opacity-value').textContent=$('refinement-opacity').value+'%';$('refinement-image').style.opacity=Number($('refinement-opacity').value)/100;};$('refinement-preset').onchange=()=>{const preset=selectedRefinementPreset();$('refinement-preset-name').value=preset?.name||'';$('refinement-preset-delete').disabled=!preset;compareRefinementParameters();};$('refinement-preset-load').onclick=loadSelectedRefinementPreset;$('refinement-preset-save').onclick=saveRefinementPreset;$('refinement-preset-delete').onclick=deleteRefinementPreset;$('refinement-restore-defaults').onclick=()=>{renderRefinementPresets();writeRefinementParameters(refinement.defaults);};Object.values(refinementParameterFields).forEach(id=>$(id).addEventListener('input',compareRefinementParameters));

// -----------------------------------------------------------------------------
// USER MANAGEMENT & ACCESS CONTROL (ADMIN ONLY)
// -----------------------------------------------------------------------------
let allUsers = [];
let auditEvents = [];

async function loadUsers(){
  const container = $('users-table-container');
  const auditContainer = $('audit-table-container');
  if(!container) return;
  try{
    const [uRes, aRes] = await Promise.all([
      api('/api/v1/admin/users'),
      api('/api/v1/admin/audit')
    ]);
    allUsers = uRes.data || [];
    auditEvents = aRes.data || [];

    const total = allUsers.length;
    const active = allUsers.filter(u => u.is_active).length;
    const admins = allUsers.filter(u => u.role === 'admin').length;
    if($('stat-users-total')) $('stat-users-total').textContent = number.format(total);
    if($('stat-users-active')) $('stat-users-active').textContent = number.format(active);
    if($('stat-users-admins')) $('stat-users-admins').textContent = number.format(admins);
    if($('stat-audit-count')) $('stat-audit-count').textContent = number.format(auditEvents.length);

    renderUsersTable();
    renderAuditTable();
  }catch(err){
    container.innerHTML = `<div class="empty"><h3>Erro ao carregar usuários</h3><p>${h(err.message)}</p></div>`;
    if(auditContainer) auditContainer.innerHTML = `<div class="empty"><h3>Erro ao carregar auditoria</h3><p>${h(err.message)}</p></div>`;
  }
}

function renderUsersTable(){
  const container = $('users-table-container');
  if(!container) return;
  const search = ($('user-search')?.value || '').trim().toLowerCase();
  const roleFilter = $('user-role-filter')?.value || 'all';
  const statusFilter = $('user-status-filter')?.value || 'all';

  const filtered = allUsers.filter(u => {
    const matchesSearch = !search || [u.email, u.name, u.id].join(' ').toLowerCase().includes(search);
    const matchesRole = (roleFilter === 'all' || u.role === roleFilter);
    const matchesStatus = (statusFilter === 'all' || (statusFilter === 'active' && u.is_active) || (statusFilter === 'inactive' && !u.is_active));
    return matchesSearch && matchesRole && matchesStatus;
  });

  if(!filtered.length){
    container.innerHTML = `<div class="empty"><h3>Nenhum usuário encontrado</h3><p>Ajuste os filtros de pesquisa ou cadastre um novo usuário.</p></div>`;
    return;
  }

  let html = `<table class="admin-table">
    <thead>
      <tr>
        <th>Usuário</th>
        <th>Papel</th>
        <th>Status</th>
        <th>Último Acesso</th>
        <th>Criado em</th>
        <th style="text-align:right;">Ações</th>
      </tr>
    </thead>
    <tbody>`;

  for(const u of filtered){
    const isSelf = currentUser && currentUser.email === u.email;
    const roleNames = {admin: 'Administrador', researcher: 'Pesquisador', user: 'Usuário'};
    const roleClass = u.role || 'user';
    const roleText = roleNames[u.role] || u.role;
    const lastLogin = u.last_login_at ? new Date(u.last_login_at).toLocaleString('pt-BR') : 'Nunca acessou';
    const createdAt = u.created_at ? u.created_at.slice(0, 10) : '—';

    html += `<tr>
      <td>
        <div class="user-cell">
          <span class="user-avatar" aria-hidden="true">${h(getInitials(u.name, u.email))}</span>
          <div class="user-info-text">
            <strong>${h(u.name || u.email)} ${isSelf ? '<small style="color:var(--blue);font-weight:800;">(Você)</small>' : ''}</strong>
            <span>${h(u.email)}</span>
          </div>
        </div>
      </td>
      <td>
        <span class="role-badge ${h(roleClass)}">${h(roleText)}</span>
      </td>
      <td>
        <span class="status-badge ${u.is_active ? 'active' : 'inactive'}">${u.is_active ? 'Ativo' : 'Inativo'}</span>
      </td>
      <td><small style="color:var(--navy);font-weight:600;">${h(lastLogin)}</small></td>
      <td><small style="color:var(--muted);">${h(createdAt)}</small></td>
      <td style="text-align:right;">
        <div class="admin-actions" style="justify-content:flex-end;">
          <button type="button" class="admin-btn" data-edit-user="${h(u.email)}">Editar</button>
          ${isSelf ? '<button type="button" class="admin-btn" disabled title="Não é permitido desativar sua própria conta">Ativo</button>' : `
            <button type="button" class="admin-btn ${u.is_active ? 'danger' : 'success'}" data-toggle-user="${h(u.email)}" data-new-status="${!u.is_active}">
              ${u.is_active ? 'Desativar' : 'Ativar'}
            </button>
          `}
        </div>
      </td>
    </tr>`;
  }

  html += `</tbody></table>`;
  container.innerHTML = html;

  for(const btn of container.querySelectorAll('[data-edit-user]')){
    btn.onclick = () => openEditUserModal(btn.dataset.editUser);
  }
  for(const btn of container.querySelectorAll('[data-toggle-user]')){
    btn.onclick = () => toggleUserStatus(btn.dataset.toggleUser, btn.dataset.newStatus === 'true');
  }
}

function renderAuditTable(){
  const container = $('audit-table-container');
  if(!container) return;
  if(!auditEvents.length){
    container.innerHTML = `<div class="empty"><h3>Nenhum registro de auditoria</h3><p>Eventos de solicitação e validação de chaves serão exibidos aqui.</p></div>`;
    return;
  }
  let html = `<table class="admin-table">
    <thead>
      <tr>
        <th>Horário</th>
        <th>E-mail</th>
        <th>IP</th>
        <th>Ação</th>
        <th>Decisão</th>
        <th>Detalhes</th>
      </tr>
    </thead>
    <tbody>`;

  for(const a of auditEvents){
    const timeStr = a.occurred_at ? new Date(a.occurred_at).toLocaleString('pt-BR') : a.occurred_at;
    html += `<tr>
      <td><small style="font-family:ui-monospace,monospace;color:var(--navy);font-weight:700;">${h(timeStr)}</small></td>
      <td><strong>${h(a.email)}</strong></td>
      <td><small style="font-family:ui-monospace,monospace;color:var(--muted);">${h(a.ip_address || '—')}</small></td>
      <td><span style="font-weight:750;font-size:.76rem;">${h(a.action)}</span></td>
      <td><span class="audit-decision ${h(a.decision)}">${h(a.decision)}</span></td>
      <td><small style="color:var(--muted);">${h(a.details || '')}</small></td>
    </tr>`;
  }
  html += `</tbody></table>`;
  container.innerHTML = html;
}

async function toggleUserStatus(email, newStatus){
  try{
    await api('/api/v1/admin/users/status', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({email, is_active: newStatus})
    });
    toast(`Usuário ${email} ${newStatus ? 'ativado' : 'desativado'} com sucesso.`);
    await loadUsers();
  }catch(err){
    toast(err.message);
  }
}

function openNewUserModal(){
  $('user-modal-title').textContent = 'Cadastrar Novo Usuário';
  $('user-form-email').value = '';
  $('user-form-email').disabled = false;
  $('user-form-name').value = '';
  $('user-form-role').value = 'researcher';
  setUserFormFeedback('');
  $('user-modal').classList.remove('hidden');
  $('user-form-email').focus();
}

function openEditUserModal(email){
  const u = allUsers.find(x => x.email === email);
  if(!u) return;
  $('user-modal-title').textContent = 'Editar Usuário';
  $('user-form-email').value = u.email;
  $('user-form-email').disabled = true;
  $('user-form-name').value = u.name || '';
  $('user-form-role').value = u.role || 'user';
  setUserFormFeedback('');
  $('user-modal').classList.remove('hidden');
  $('user-form-name').focus();
}

function closeUserModal(){
  $('user-modal').classList.add('hidden');
  setUserFormFeedback('');
}

function setUserFormFeedback(msg, type = 'error'){
  const box = $('user-form-feedback');
  if(!box) return;
  if(!msg){ box.classList.add('hidden'); box.textContent = ''; return; }
  box.className = `auth-feedback ${type}`;
  box.textContent = msg;
  box.classList.remove('hidden');
}

async function saveUser(e){
  e.preventDefault();
  const email = $('user-form-email').value.trim().toLowerCase();
  const name = $('user-form-name').value.trim();
  const role = $('user-form-role').value;
  const btn = $('btn-save-user');
  btn.disabled = true;
  setUserFormFeedback('');
  try{
    const res = await api('/api/v1/admin/users', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({email, name, role})
    });
    toast(res.message || 'Usuário salvo com sucesso!');
    closeUserModal();
    await loadUsers();
  }catch(err){
    setUserFormFeedback(err.message, 'error');
  }finally{
    btn.disabled = false;
  }
}

// Wire admin listeners
if($('btn-open-new-user')) $('btn-open-new-user').onclick = openNewUserModal;
if($('close-user-modal')) $('close-user-modal').onclick = closeUserModal;
if($('btn-cancel-user')) $('btn-cancel-user').onclick = closeUserModal;
if($('user-form')) $('user-form').onsubmit = saveUser;
if($('refresh-users')) $('refresh-users').onclick = loadUsers;
if($('refresh-audit')) $('refresh-audit').onclick = loadUsers;
if($('user-search')) $('user-search').oninput = renderUsersTable;
if($('user-role-filter')) $('user-role-filter').onchange = renderUsersTable;
if($('user-status-filter')) $('user-status-filter').onchange = renderUsersTable;
