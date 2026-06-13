"use strict";

async function api(action, id) {
  const parameters = new URLSearchParams({action});
  if (id) parameters.set("id", id);
  const response = await fetch(`/cgi-bin/tsched-api?${parameters}`);
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  return response.json();
}

async function refresh() {
  const status = document.querySelector("#status");
  try {
    const data = await api("tasks");
    status.textContent = `${data.running || 0} 个任务运行中`;
    document.querySelector("#tasks").innerHTML = (data.tasks || []).map(task => `
      <tr><td>${task.id}</td><td>${task.name}</td><td>${task.enabled ? "是" : "否"}</td>
      <td>${task.state}</td><td>${task.runs}</td>
      <td><button data-run="${task.id}">运行</button></td></tr>`).join("");
  } catch (error) {
    status.textContent = `连接失败：${error.message}`;
  }
}

document.querySelector("#refresh").addEventListener("click", refresh);
document.addEventListener("click", async event => {
  if (event.target.dataset.run) {
    await api("run", event.target.dataset.run);
    refresh();
  }
});
refresh();
setInterval(refresh, 5000);
