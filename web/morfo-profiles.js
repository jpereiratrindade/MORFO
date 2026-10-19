/* MORFO-UX-002 — thematic workbench; no biological labels are inferred by this UI.
   Existing MORFO protected API and original EXP-003 algorithm remain unchanged.
   GPL-3.0-or-later; derived from MORFO/RIZOMA UI architecture. */
(() => {
  'use strict';

  const $ = id => document.getElementById(id);
  const number = new Intl.NumberFormat('pt-BR');
  const state = { imageId:null, imageMeta:null, runs:[], run:null, report:null, metrics:null, metricRuns:[], calibrations:[],
                  layer:'original', opacity:0.85, generation:0, busy:false, profileAvailable:true };
  const profile = 'roots-exp003';
  const rootPath = id => `/api/v1/images/${encodeURIComponent(id)}/profiles/${profile}`;
  const artifactPath = (id,run,kind) => `${rootPath(id)}/${encodeURIComponent(run)}/${kind}`;
  const metricsPath = (id,run) => `${rootPath(id)}/${encodeURIComponent(run)}/metrics`;
  const metricsExportPath = (id,run,format) => `${metricsPath(id,run)}/export.${format}`;
  const calibrationPath = id => `/api/v1/images/${encodeURIComponent(id)}/calibrations`;
  let workspace, layer, panel;

  function node(tag, props={}, children=[]) {
    const el=document.createElement(tag);
    for (const [k,v] of Object.entries(props)) {
      if (k==='text') el.textContent=v;
      else if (k==='className') el.className=v;
      else if (k==='type') el.type=v;
      else if (k==='id') el.id=v;
      else if (k==='title') el.title=v;
      else if (k==='href') el.href=v;
      else if (k==='download') el.download=v;
      else el.setAttribute(k,v);
    }
    for(const child of children) el.append(child);
    return el;
  }

  async function json(url,options={}) {
    const r=await fetch(url,{credentials:'same-origin',cache:'no-store',...options});
    const result=await r.json().catch(()=>null);
    if(!r.ok)throw Error((result && result.error) || `HTTP ${r.status}`);
    if(!result)throw Error('Resposta não contém JSON válido');
    return result;
  }

  function message(value,level='normal') {
    const el=$('morfo-workbench-status');
    if(el){el.textContent=value;el.dataset.level=level;}
  }

  function showPanelPlaceholder(value) {
    if(!panel)return;
    panel.replaceChildren(node('div',{className:'morfo-empty',text:value}));
  }

  function clearVisual() {
    if(layer){layer.removeAttribute('src');layer.classList.add('hidden');layer.style.opacity='1';}
    state.run=null;state.report=null;state.metrics=null;state.metricRuns=[];state.layer='original';
    refreshLayers();
  }

  function renderPanel() {
    if(!panel)return;
    const run=state.run, report=state.report;
    if(!run || !report) {
      showPanelPlaceholder('Abra uma execução existente ou processe a imagem com o perfil Raízes — EXP-003. Nenhuma raiz foi confirmada.');
      return;
    }
    const wrap=node('section',{className:'morfo-result-info'});
    const eyebrow=node('p',{className:'morfo-eyebrow',text:'EVIDÊNCIA COMPUTACIONAL'});
    const title=node('h3',{text:'Resultado do perfil'});
    const status=node('p',{className:'morfo-validation',text:'NÃO VALIDADO · candidatos de anomalia'});
    const dl=node('dl',{className:'morfo-stat-list'});
    const values=[
      ['Perfil',`${report.profile||profile} · ${report.version||'EXP-003'}`],
      ['Pixels candidatos',number.format(report.candidate_pixels ?? run.candidate_pixels)],
      ['Componentes conexos',number.format(report.components ?? run.components)],
      ['Pixels abstidos nas bordas',number.format(report.border_abstentions ?? run.border_abstentions)],
      ['Classificação',report.classification||'ANOMALY_CANDIDATE'],
      ['Identidade biológica','DESCONHECIDA'],
      ['Data da execução',run.created_at||'Não informada'],
      ['ID da execução',run.id]
    ];
    for(const [key,value] of values){dl.append(node('dt',{text:key}),node('dd',{text:String(value)}));}
    const dl2=node('dl',{className:'morfo-hashes'});
    dl2.append(node('dt',{text:'SHA-256 da imagem'}),node('dd',{text:report.original_sha256||'Indisponível'}),
               node('dt',{text:'SHA-256 da máscara'}),node('dd',{text:report.mask_sha256||'Indisponível'}));
    const links=node('nav',{className:'morfo-artifacts','aria-label':'Artefatos da execução'});
    for(const [kind,label] of [['mask','Máscara PNG'],['overlay','Sobreposição PNG'],['report','Relatório JSON']]){
      const a=node('a',{href:artifactPath(state.imageId,run.id,kind),text:label});
      a.target='_blank';a.rel='noopener';links.append(a);
    }
    const metrics=renderMetrics();
    const note=node('p',{className:'morfo-method-note',text:'Morfometria descreve candidatos bidimensionais do EXP-003; não confirma identidade biológica. Comprimento projetado está disponível no Gate 001B; diâmetro e topologia pertencem ao Gate 001C.'});
    const planned=node('p',{className:'morfo-planned',text:'Revisão pixel a pixel e descoberta geral de categorias: planejadas, ainda indisponíveis.'});
    wrap.append(eyebrow,title,status,dl,dl2,links,metrics,note,planned);
    panel.replaceChildren(wrap);
  }

  function renderMetrics(){
    const section=node('section',{className:'root-metrics'});
    section.append(node('p',{className:'morfo-eyebrow',text:'RAÍZES → MORFOMETRIA'}),node('h3',{text:'Área, comprimento e topologia · Gate 001C'}));
    const form=node('form',{id:'root-metrics-form',className:'root-metrics-form'});
    form.innerHTML=`<label>Escala X <span>mm/pixel</span><input id="metric-scale-x" type="number" min="0" step="any" placeholder="opcional"></label>
      <label>Escala Y <span>mm/pixel</span><input id="metric-scale-y" type="number" min="0" step="any" placeholder="opcional"></label>
      <label class="metric-calibration-picker">Calibração salva <select id="metric-calibration"><option value="">Sem calibração física</option>${state.calibrations.map(c=>`<option value="${c.id}">${c.source_type} · ${c.scale_x_mm_per_pixel} × ${c.scale_y_mm_per_pixel} mm/px${c.verified?' · verificada':' · indicação'}</option>`).join('')}</select></label>
      <label class="metric-calibration-picker">Origem da escala <select id="metric-calibration-source"><option value="MANUAL">Informada manualmente</option><option value="IMAGE_REFERENCE">Referência física na imagem</option><option value="EQUIPMENT_METADATA">Indicação dos metadados/equipamento</option></select></label>
      <button type="button" id="metric-save-calibration" class="metric-secondary">Salvar indicação de escala</button>
      <button type="submit" class="morfo-run">Calcular morfometria</button>`;
    form.querySelector('#metric-calibration').addEventListener('change',event=>{const c=state.calibrations.find(item=>item.id===event.target.value);$('metric-scale-x').value=c?.scale_x_mm_per_pixel??'';$('metric-scale-y').value=c?.scale_y_mm_per_pixel??'';});
    form.querySelector('#metric-save-calibration').addEventListener('click',saveCalibration);
    form.addEventListener('submit',startMetrics);section.append(form);
    if(state.metricRuns.length){const history=node('label',{className:'metric-history'});history.append(node('span',{text:'Histórico morfométrico'}));const select=node('select',{id:'metric-run-history'});for(const run of state.metricRuns)select.append(node('option',{value:run.id,text:`${run.created_at} · ${run.status} · ${run.id.slice(0,8)}`}));select.value=state.metrics?.id||'';select.addEventListener('change',()=>loadMetricRun(select.value));history.append(select);section.append(history);}
    const m=state.metrics;
    if(!m){section.append(node('p',{className:'root-metrics-empty',text:'Nenhuma execução morfométrica para esta máscara.'}));return section;}
    const stats=node('dl',{className:'root-metrics-stats'});
    const fmt=(v,d=2)=>v==null?'Indisponível':Number(v).toLocaleString('pt-BR',{maximumFractionDigits:d});
    const values=[['Área candidata',m.projected_area_mm2==null?`${number.format(m.candidate_pixels)} px²`:`${fmt(m.projected_area_mm2,4)} mm²`],
      ['Comprimento projetado',m.projected_length_mm==null?`${fmt(m.projected_length_px,3)} px`:`${fmt(m.projected_length_mm,4)} mm`],
      ['Ocupação',m.occupancy_percent==null?'Indisponível':`${fmt(m.occupancy_percent)}%`],['Componentes',number.format(m.components?.length||0)],
      ['Pixels do esqueleto',number.format(m.skeleton_pixels||0)],['Arestas do grafo',number.format(m.graph_edges||0)],
      ['Pontas aparentes',number.format(m.apparent_tips||0)],['Ramificações aparentes',number.format(m.apparent_branches||0)],
      ['Pixels isolados',number.format(m.isolated_skeleton_pixels||0)],['Pontas truncadas',number.format(m.truncated_tips||0)],
      ['Pixels válidos',number.format(m.valid_pixels)],['Matriz',number.format(m.matrix_pixels)],['Excluídos/abstidos',number.format(m.excluded_or_abstained_pixels)]];
    for(const [key,value] of values)stats.append(node('dt',{text:key}),node('dd',{text:value}));section.append(stats);
    const table=node('div',{className:'root-metrics-table-wrap'});table.innerHTML='<table><thead><tr><th>ID</th><th>Área</th><th>Comprimento</th><th>Centroide</th><th>Limites</th></tr></thead><tbody></tbody></table>';
    const tbody=table.querySelector('tbody');for(const c of (m.components||[]).slice(0,100)){
      const tr=node('tr');for(const value of [c.id,`${number.format(c.area_px)} px²`,c.projected_length_mm==null?`${fmt(c.projected_length_px,2)} px`:`${fmt(c.projected_length_mm,3)} mm`,`${fmt(c.centroid_x_px,1)}, ${fmt(c.centroid_y_px,1)}`,`${c.bounding_box.min_x},${c.bounding_box.min_y}–${c.bounding_box.max_x},${c.bounding_box.max_y}`])tr.append(node('td',{text:String(value)}));tbody.append(tr);
    }section.append(table);
    const exports=node('nav',{className:'root-metrics-exports','aria-label':'Exportar dados morfométricos'});
    for(const [format,label] of [['csv','Exportar CSV'],['json','Exportar JSON']])exports.append(node('a',{href:metricsExportPath(state.imageId,state.run.id,format),download:`morfo-morfometria-${state.run.id}.${format}`,text:label}));
    section.append(exports);
    const record=node('form',{className:'metric-record-form'});record.innerHTML=`<label>Estado<select id="metric-record-status"><option>NOT_VALIDATED</option><option>REVIEWED</option><option>ARCHIVED</option></select></label><label>Notas<input id="metric-record-notes" maxlength="2000" value=""></label><button type="submit">Salvar registro</button><button type="button" class="metric-delete">Excluir</button>`;
    const listed=state.metricRuns.find(item=>item.id===m.id);record.querySelector('#metric-record-status').value=m.record_status||listed?.status||'NOT_VALIDATED';record.querySelector('#metric-record-notes').value=m.notes||listed?.notes||'';record.addEventListener('submit',updateMetricRecord);record.querySelector('.metric-delete').addEventListener('click',deleteMetricRecord);section.append(record);
    section.append(node('p',{className:'root-metrics-provenance',text:`${m.scientific_status} · ${m.metrics_version} · ${m.id}`}));return section;
  }

  async function startMetrics(event){
    event.preventDefault();if(!state.run||state.busy)return;
    const x=$('metric-scale-x').value.trim(),y=$('metric-scale-y').value.trim();
    if((x&&!y)||(!x&&y)){message('Informe ambas escalas X e Y, ou deixe ambas vazias.','error');return;}
    const submitter=event.submitter;state.busy=true;if(submitter)submitter.disabled=true;message('Calculando área, ocupação e componentes na resolução original...');
    const selected=state.calibrations.find(item=>item.id===$('metric-calibration').value);
    try{const response=await json(metricsPath(state.imageId,state.run.id),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({scale_x:x?Number(x):null,scale_y:y?Number(y):null,origin:selected?.source_type||(x?'MANUAL':''),method:selected?.method||(x?'manual':''),date:new Date().toISOString(),version:selected?.version||'ui-001a',responsible:selected?.responsible||''})});state.metrics=response.data;await loadMetricLists();renderPanel();message('Morfometria Gate 001A calculada e persistida.');}
    catch(error){message(`Falha na morfometria: ${error.message}`,'error');}
    finally{state.busy=false;if(submitter&&submitter.isConnected)submitter.disabled=false;}
  }

  async function loadMetricLists(){if(!state.imageId||!state.run)return;const [runs,calibrations]=await Promise.all([json(`${metricsPath(state.imageId,state.run.id)}/runs`),json(calibrationPath(state.imageId))]);state.metricRuns=runs.data||[];state.calibrations=calibrations.data||[];}
  async function loadMetricRun(id){try{const response=await json(`/api/v1/root-metrics/${encodeURIComponent(id)}`);state.metrics=response.data;renderPanel();}catch(error){message(`Falha ao abrir registro: ${error.message}`,'error');}}
  async function updateMetricRecord(event){event.preventDefault();try{const response=await json(`/api/v1/root-metrics/${encodeURIComponent(state.metrics.id)}`,{method:'PATCH',headers:{'Content-Type':'application/json'},body:JSON.stringify({status:$('metric-record-status').value,notes:$('metric-record-notes').value})});state.metrics=response.data;await loadMetricLists();renderPanel();message('Registro morfométrico atualizado.');}catch(error){message(`Falha ao atualizar: ${error.message}`,'error');}}
  async function deleteMetricRecord(){if(!state.metrics||!confirm('Excluir esta execução morfométrica do histórico ativo?'))return;try{await json(`/api/v1/root-metrics/${encodeURIComponent(state.metrics.id)}`,{method:'DELETE'});state.metrics=null;await loadMetricLists();if(state.metricRuns.length)await loadMetricRun(state.metricRuns[0].id);else renderPanel();message('Execução removida do histórico ativo.');}catch(error){message(`Falha ao excluir: ${error.message}`,'error');}}
  async function saveCalibration(){const x=$('metric-scale-x').value.trim(),y=$('metric-scale-y').value.trim();if(!x||!y){message('Informe escalas X e Y para salvar.','error');return;}const source=$('metric-calibration-source').value;try{const response=await json(calibrationPath(state.imageId),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({scale_x:Number(x),scale_y:Number(y),source_type:source,source_detail:source==='EQUIPMENT_METADATA'?'Indicação declarada a partir dos metadados/equipamento':'Informada na interface',method:source.toLowerCase(),version:'ui-001a',responsible:'',verified:false,notes:source==='EQUIPMENT_METADATA'?'Requer verificação do significado metrológico dos metadados.':''})});state.calibrations=response.data||[];renderPanel();message('Escala salva como indicação não verificada.');}catch(error){message(`Falha ao salvar calibração: ${error.message}`,'error');}}

  function refreshLayers() {
    if(!workspace)return;
    for(const button of workspace.querySelectorAll('[data-morfo-layer]')) {
      const active=button.dataset.morfoLayer===state.layer;
      button.classList.toggle('active',active);
      button.setAttribute('aria-pressed',String(active));
      button.disabled=!state.run && button.dataset.morfoLayer!=='original';
    }
    const slider=$('morfo-opacity');
    if(slider){slider.value=String(Math.round(state.opacity*100));slider.disabled=!state.run || state.layer!=='overlay';}
    const pct=$('morfo-opacity-value');if(pct)pct.textContent=`${Math.round(state.opacity*100)}%`;
    if(layer){layer.style.opacity=String(state.layer==='mask'?1:state.opacity);layer.classList.toggle('hidden',state.layer==='original'||!state.run);}
  }

  function showLayer(kind) {
    if(kind!=='original'&&!state.run) return;
    if(!['original','mask','overlay'].includes(kind))return;
    state.layer=kind;
    refreshLayers();
    if(!layer || kind==='original')return;
    const expected=artifactPath(state.imageId,state.run.id,kind);
    // The overlay lives within #analysis-canvas, the very same transformed image coordinate system.
    layer.src=expected;
  }

  async function readMask(runId,expectedReport,imageId,epoch){
    const preview=new Image();
    preview.decoding='async';
    preview.src=artifactPath(imageId,runId,'mask');
    await new Promise((resolve,reject)=>{preview.onload=resolve;preview.onerror=()=>reject(Error('Não foi possível carregar a máscara PNG'));
      if(preview.complete){if(preview.naturalWidth)resolve();}
    });
    if(epoch!==state.generation||imageId!==state.imageId)return false;
    const w=Number(expectedReport.width),h=Number(expectedReport.height);
    if(!w||!h||preview.naturalWidth!==w||preview.naturalHeight!==h)throw Error('Dimensões da máscara não correspondem ao relatório');
    if(state.imageMeta && (w!==Number(state.imageMeta.width)||h!==Number(state.imageMeta.height)))throw Error('Máscara não corresponde às dimensões do original');
    if(state.imageMeta?.sha256 && expectedReport.original_sha256!==state.imageMeta.sha256)throw Error('Hash do original não corresponde ao relatório da execução');
    return true;
  }

  async function selectRun(id) {
    const imageId=state.imageId;
    const epoch=++state.generation;
    clearVisual();
    if(!id){message('Nenhuma execução selecionada.');renderPanel();return;}
    const run=state.runs.find(r=>r.id===id);
    if(!run)throw Error('Execução indisponível no histórico');
    message('Verificando artefatos e alinhamento com o original...');
    try{
      const report=await json(artifactPath(imageId,id,'report'));
      if(epoch!==state.generation || imageId!==state.imageId)return;
      const good=await readMask(id,report,imageId,epoch);
      if(!good)return;
      state.run=run;state.report=report;
      let metricResponse=await json(metricsPath(imageId,id));
      if(epoch!==state.generation||imageId!==state.imageId)return;
      if(!metricResponse.data){
        message('Primeira abertura: calculando morfometria determinística sem escala física...');
        metricResponse=await json(metricsPath(imageId,id),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({scale_x:null,scale_y:null,origin:'',method:'',date:new Date().toISOString(),version:'auto-001b',responsible:''})});
        if(epoch!==state.generation||imageId!==state.imageId)return;
      }
      state.metrics=metricResponse.data;
      await loadMetricLists();
      renderPanel();showLayer('overlay');
      message(`Execução ${id} carregada. Candidatos visuais; validação biológica pendente.`);
    }catch(error){
      if(epoch!==state.generation)return;
      clearVisual();renderPanel();message(`Falha na consulta: ${error.message}`,'error');
    }
  }

  async function loadHistory(imageId,autoSelect=true) {
    if(!imageId || state.imageId!==imageId)return;
    const epoch=++state.generation;
    try {
      const resp=await json(rootPath(imageId));
      if(epoch!==state.generation || imageId!==state.imageId)return;
      state.runs=Array.isArray(resp.data)?resp.data:[];
      const picker=$('morfo-history');
      picker.replaceChildren(node('option',{value:'',text:state.runs.length?'Selecione uma execução':'Nenhuma execução registrada'}));
      for(const run of state.runs){
        const title=`${run.created_at||'Sem data'} · ${number.format(run.candidate_pixels)} candidatos · ${run.id.slice(0,8)}`;
        picker.append(node('option',{value:run.id,text:title}));
      }
      picker.disabled=!state.runs.length;
      if(autoSelect && state.runs.length){picker.value=state.runs[0].id;await selectRun(picker.value);}
      else {clearVisual();renderPanel();message('Perfil disponível. Nenhum resultado foi assumido como validação biológica.');}
    }catch(err){if(epoch===state.generation)message(`Falha ao consultar histórico: ${err.message}`,'error');}
  }

  async function startProfile() {
    const imageId=state.imageId;
    if(!imageId || state.busy || !state.profileAvailable)return;
    state.busy=true;
    const button=$('morfo-run');button.disabled=true;
    message('Executando EXP-003 sobre o original preservado...');
    try{
      const out=await json(rootPath(imageId),{method:'POST'});
      if(imageId!==state.imageId)return;
      const id=out.data?.id;
      if(!id)throw Error('O servidor não retornou ID de execução');
      await loadHistory(imageId,false);
      const picker=$('morfo-history');
      picker.value=id;
      await selectRun(id);
    }catch(error){if(imageId===state.imageId)message(`Falha: ${error.message}`,'error');}
    finally{state.busy=false;if(button)button.disabled=!state.imageId || !state.profileAvailable;}
  }

  function build() {
    const inspector=$('inspector');
    const tools=inspector?.querySelector('.modal-tools');
    panel=$('analysis-panel');
    const canvas=$('analysis-canvas');
    if(!tools || !panel || !canvas)throw Error('Interface MORFO incompatível: componentes do visualizador ausentes');
    workspace=node('section',{id:'morfo-thematic-workspace',className:'morfo-thematic-workspace','aria-label':'Análise temática'});
    workspace.innerHTML=`
      <div class="morfo-head"><div><p class="morfo-eyebrow">AMBIENTE DE PERCEPÇÃO VISUAL</p><h3>Perfis temáticos</h3>
      <p>Selecione o método, processe e inspecione as evidências na imagem completa.</p></div>
      <span class="morfo-state">PERFIL EXPERIMENTAL</span></div>
      <div class="morfo-command-bar">
        <label for="morfo-theme">Perfil<select id="morfo-theme">
          <option value="roots-exp003">Raízes · Matriz adaptativa EXP-003</option>
          <option value="discovery" disabled>Descoberta geral REA/RIT · planejada</option>
        </select></label>
        <button id="morfo-run" type="button" class="morfo-run">Executar perfil</button>
        <label for="morfo-history">Histórico de execuções<select id="morfo-history" disabled>
          <option value="">Nenhuma execução</option></select></label>
      </div>
      <div class="morfo-layer-controls" role="group" aria-label="Camadas do resultado">
        <span class="morfo-layer-label">Visualização</span>
        <button type="button" data-morfo-layer="original" aria-pressed="true">Original</button>
        <button type="button" data-morfo-layer="mask" aria-pressed="false" disabled>Máscara binária</button>
        <button type="button" data-morfo-layer="overlay" aria-pressed="false" disabled>Sobreposição</button>
        <label class="morfo-opacity">Opacidade da sobreposição <input id="morfo-opacity" type="range" min="0" max="100" value="85" disabled>
          <output id="morfo-opacity-value">85%</output></label>
      </div>
      <div id="morfo-workbench-status" class="morfo-status" aria-live="polite">Abra uma imagem para consultar seus perfis.</div>`;
    tools.insertAdjacentElement('afterend',workspace);
    layer=node('img',{id:'morfo-visual-layer',className:'morfo-visual-layer hidden',alt:'Resultado do perfil visual, alinhado às coordenadas do original','aria-hidden':'true'});
    layer.draggable=false;
    canvas.append(layer);
    $('morfo-run').addEventListener('click',startProfile);
    $('morfo-history').addEventListener('change',e=>selectRun(e.target.value));
    workspace.querySelectorAll('[data-morfo-layer]').forEach(b=>b.addEventListener('click',()=>showLayer(b.dataset.morfoLayer)));
    $('morfo-opacity').addEventListener('input',e=>{state.opacity=Number(e.target.value)/100;refreshLayers();});
    refreshLayers();renderPanel();
  }

  async function verifyRegistry() {
    try {
      const catalog=await json('/api/v1/profiles');
      const rootProfile=catalog.data?.find(p=>p.id==='roots-exp003');
      if(!rootProfile || rootProfile.status==='PLANNED'){
        state.profileAvailable=false;
        $('morfo-run').disabled=true;
        message('O servidor não declarou o perfil EXP-003 como implementado.','error');
      }
    }catch(err){state.profileAvailable=false;$('morfo-run').disabled=true;message(`Catálogo de perfis indisponível: ${err.message}`,'error');}
  }

  function onImageOpened(event){
    const imageId=event.detail?.imageId || window.MORFO_Bridge?.activeImageId?.();
    state.generation++;
    state.imageId=imageId||null;
    state.imageMeta=window.MORFO_Bridge?.activeImageMeta?.()||null;
    state.runs=[];state.calibrations=[];
    state.busy=false;
    const picker=$('morfo-history');
    picker.replaceChildren(node('option',{value:'',text:'Consultando histórico...'}));picker.disabled=true;
    $('morfo-run').disabled=!imageId || !state.profileAvailable;
    clearVisual();renderPanel();
    if(imageId)loadHistory(imageId);
  }

  function onImageClosed(){
    state.generation++;
    state.imageId=null;state.imageMeta=null;state.runs=[];state.calibrations=[];state.busy=false;
    clearVisual();renderPanel();
  }

  document.addEventListener('DOMContentLoaded',()=>{
    build();verifyRegistry();
    window.addEventListener('morfo:image-opened',onImageOpened);
    window.addEventListener('morfo:image-closed',onImageClosed);
  });
})();
