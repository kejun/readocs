#pragma once
namespace rd {
static constexpr char PAGE[] = R"HTML(<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Readocs · Wi-Fi</title>
<style>*{box-sizing:border-box}body{font:16px/1.6 -apple-system,BlinkMacSystemFont,"PingFang SC",sans-serif;color:#1c252c;background:#f7f8fa;max-width:440px;padding:32px 24px;margin:20px auto}h1{font-size:28px;margin:40px 0 30px}label{display:block;font-size:14px;margin-top:22px}input,select,button{font:inherit;width:100%;padding:12px;border:1px solid #d3dde1;border-radius:7px;background:white;margin-top:8px}button{cursor:pointer}button:disabled{opacity:.55;cursor:wait}button[type=submit]{background:#17766b;color:white;border:0;margin-top:30px}#networks button{text-align:left;background:none;border:0;border-bottom:1px solid #e0e6e9;border-radius:0}details{font-size:13px;margin-top:28px;color:#69777e}#result{font-size:14px;white-space:pre-line}header{font-weight:650;font-size:22px}</style>
<header>readocs</header><h1>连接 Wi-Fi</h1>
<div id="networks"></div><button id="scan" disabled>扫描网络</button>
<form id="form"><label for="ssid">Wi-Fi 名称（也可手动填写隐藏网络）</label>
<input id="ssid" maxlength="32" autocomplete="off" required>
<label for="password">密码</label><input id="password" type="password" maxlength="63" autocomplete="new-password">
<div id="replace" hidden><label for="replacement">已保存 5 组，请选择要替换的网络</label><select id="replacement"></select></div>
<button type="submit" id="connect" disabled>连接</button><p id="result" role="status" aria-live="polite"></p></form>
<details><summary>已保存的网络</summary><div id="saved"></div></details>
<details><summary>网络要求</summary><p>使用 2.4 GHz 个人 Wi-Fi 或手机热点。换网络后，阅读内容和飞书配置会保留。这里仅确认 Wi-Fi 连接和保存，代理及飞书授权状态请查看设备。</p></details>
<script>
let session='', profiles=[], busy=false;
const $=s=>document.querySelector(s);
function message(text){$('#result').textContent=text}
function setBusy(value){busy=value;document.querySelectorAll('input,select,button').forEach(e=>e.disabled=value)}
function replacement(){
  $('#replace').hidden=profiles.length<5||profiles.includes($('#ssid').value);
}
async function call(path,data){
  const abort=new AbortController(), timer=setTimeout(()=>abort.abort(),4000);
  try{
    const r=await fetch(path,{method:data?'POST':'GET',cache:'no-store',signal:abort.signal,
      headers:data?{'Content-Type':'application/json','X-Setup-Session':session}:{},body:data?JSON.stringify(data):undefined});
    const text=await r.text();let j;
    if((r.headers.get('Content-Type')||'').includes('application/json')){
      try{j=JSON.parse(text)}catch{throw Error('设备响应不完整，请重试')}
    }else{j={message:text.trim()}}
    if(!r.ok)throw Error(j.message||'操作失败（'+r.status+'）');
    return j;
  }catch(e){if(e.name==='AbortError')throw Error('设备响应超时，请确认仍连接 Readocs 热点');throw e}
  finally{clearTimeout(timer)}
}
function showProfiles(){
  $('#saved').replaceChildren();$('#replacement').replaceChildren();
  const first=document.createElement('option');first.value='';first.textContent='请选择';$('#replacement').append(first);
  profiles.forEach(n=>{
    const option=document.createElement('option');option.value=n;option.textContent=n;$('#replacement').append(option);
    const b=document.createElement('button');b.type='button';b.textContent='忘记 '+n;
    b.onclick=async()=>{
      if(busy||!confirm('忘记 '+n+'？'))return;
      setBusy(true);
      try{const accepted=await call('/api/forget',{ssid:n});await poll(accepted.operation,false);await load()}
      catch(e){message(e.message)}finally{setBusy(false)}
    };$('#saved').append(b);
  });replacement();
}
async function poll(operation,wifi){
  const until=Date.now()+75000;
  while(Date.now()<until){
    await new Promise(resolve=>setTimeout(resolve,1000));
    let status;
    try{status=await call('/api/status')}
    catch{message('正在等待设备结果，请保持连接 Readocs 热点；热点断开不代表连接成功。');continue}
    if(status.session!==session)throw Error('配网会话已更换，请刷新页面重新操作');
    const result=status.connection;
    if(!result||result.operation!==operation)continue;
    message(result.message||'正在连接 Wi-Fi…');
    if(result.pending)continue;
    if(result.state==='failed')throw Error(result.message||'连接失败，请修改后重试');
    if(wifi&&result.state==='success'){
      message('Wi-Fi 已连接并保存。热点即将关闭，请切回原网络。\n代理和飞书授权状态请查看设备。');
      try{await call('/api/complete',{})}catch{}
      return;
    }
    if(!wifi)return;
  }
  throw Error('尚未取得明确的连接结果，请查看设备屏幕；重新连接热点后可重试。');
}
async function load(){
  const status=await call('/api/status');session=status.session;profiles=status.networks;showProfiles();
  const result=status.connection;
  if(result?.pending){setBusy(true);try{await poll(result.operation,result.kind==='wifi')}finally{setBusy(false)}}
  else{setBusy(false);if(result?.message)message(result.message)}
}
$('#ssid').oninput=replacement;
$('#scan').onclick=async()=>{
  if(busy)return;setBusy(true);
  try{
    const list=await call('/api/scan');$('#networks').replaceChildren();
    list.networks.forEach(n=>{const b=document.createElement('button');b.type='button';b.textContent=n.ssid+(n.secure?' · 加密':'');
      b.onclick=()=>{$('#ssid').value=n.ssid;replacement();$('#password').focus()};$('#networks').append(b)});
    message(list.networks.length?'请选择网络，或手动输入隐藏网络。':'没有发现网络，可手动输入名称重试。');
  }catch(e){message(e.message)}finally{setBusy(false)}
};
$('#form').onsubmit=async e=>{
  e.preventDefault();if(busy||!session)return;
  const data={ssid:$('#ssid').value,password:$('#password').value};
  if(profiles.length>=5&&!profiles.includes(data.ssid)){
    data.replace=$('#replacement').value;
    if(!profiles.includes(data.replace)){message('请选择要替换的已保存网络');return}
  }
  setBusy(true);
  try{const accepted=await call('/api/wifi',data);$('#password').value='';message('正在连接 Wi-Fi…');await poll(accepted.operation,true)}
  catch(err){message(err.message)}finally{setBusy(false)}
};
load().catch(e=>{message(e.message+'。请重新连接热点后刷新页面重试。');setBusy(true)});
</script></html>)HTML";
} // namespace rd
