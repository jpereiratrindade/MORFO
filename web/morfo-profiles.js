/* MORFO: profile panel, separate from the inherited RIZOMA interface. */
(() => {
  'use strict';
  const root = document.createElement('div');
  root.id = 'morfo-profiles-widget';
  root.style.cssText = 'margin:10px 0;padding:12px 14px;border:1px solid #dce7ef;border-radius:12px;background:#f5f9fc;color:#18334b;display:none;max-width:700px';
  root.innerHTML = `<div style="display:flex;flex-wrap:wrap;align-items:center;gap:10px">
    <strong style="font-size:14px">MORFO · Perfis temáticos</strong>
    <select id="morfo-theme" aria-label="Perfil temático" style="padding:9px;border-radius:8px;border:1px solid #b8cbd8;min-width:230px">
      <option value="roots-exp003">Raízes · Matriz adaptativa EXP-003</option>
      <option value="discovery" disabled>Descoberta emergente (planejada)</option>
    </select>
    <button id="morfo-run" type="button" style="background:#087f8c;color:white;border:0;border-radius:8px;padding:10px 13px;cursor:pointer">Processar perfil</button>
  </div><p style="font-size:12px;margin:8px 0 0">Candidatos de anomalia — não é classificação biológica validada. Original preservado.</p>
  <div id="morfo-result" aria-live="polite" style="font-size:13px;margin-top:10px"></div>`;
  const insert = () => {
    const inspector=document.getElementById('inspector');
    if (!inspector) return;
    const target=inspector.querySelector('.modal-tools') || inspector.querySelector('.modal-head');
    if (target && root.parentNode!==target.parentNode) target.insertAdjacentElement('afterend',root);
    root.style.display=inspector.classList.contains('hidden')?'none':'block';
  };
  const status = (text) => { const target=document.getElementById('morfo-result'); if(target)target.textContent=text; };
  const button=root.querySelector('#morfo-run');
  button.addEventListener('click', async () => {
    const imageId=window.MORFO_Bridge?.activeImageId?.();
    if(!imageId){status('Abra uma imagem antes de executar um perfil.');return;}
    button.disabled=true;status('Processando o EXP-003 original...');
    try{
      const url=`/api/v1/images/${encodeURIComponent(imageId)}/profiles/roots-exp003`;
      const response=await fetch(url,{method:'POST',credentials:'same-origin'});
      const payload=await response.json();
      if(!response.ok)throw new Error(payload.error||`HTTP ${response.status}`);
      const data=payload.data;
      const target=document.getElementById('morfo-result');
      target.replaceChildren();
      const line=document.createElement('p');
      line.textContent=`Execução ${data.id} · ${data.candidate_pixels} pixels candidatos · ${data.components} componentes · ${data.scientific_status}`;
      const img=document.createElement('img');img.src=`${url}/${data.id}/overlay`;img.alt='Sobreposição experimental EXP-003 sobre a imagem';img.style.cssText='display:block;max-width:100%;max-height:320px;object-fit:contain;border-radius:8px;margin-top:8px';
      const links=document.createElement('p');links.style.cssText='display:flex;gap:14px;flex-wrap:wrap';
      for(const [key,title] of [['mask','Máscara PNG'],['report','Relatório JSON']]){
        const link=document.createElement('a');link.href=`${url}/${data.id}/${key}`;link.target='_blank';link.rel='noopener';link.textContent=title;links.append(link);
      }
      target.append(line,img,links);
    }catch(e){status(`Falha: ${e.message}`);}
    finally{button.disabled=false;}
  });
  document.addEventListener('DOMContentLoaded',()=>{
    insert();
    const inspector=document.getElementById('inspector');
    if(inspector) new MutationObserver(insert).observe(inspector,{attributes:true,attributeFilter:['class']});
  });
})();
