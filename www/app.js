"use strict";

const stateNames = ["已禁用", "等待触发", "运行中", "等待重试", "排队中"];

async function api(action, id) {
  const parameters = new URLSearchParams({action});
  if (id) parameters.set("id", id);
  const response = await fetch(`/cgi-bin/tsched-api?${parameters}`);
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  return response.json();
}

async function apiParams(parameters) {
  const response = await fetch(`/cgi-bin/tsched-api?${new URLSearchParams(parameters)}`);
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  return response.json();
}

function requireSuccess(result) {
  if (result.error) throw new Error(result.error);
  if (result.result && result.result.startsWith("ERR")) {
    throw new Error(result.result.trim());
  }
  return result;
}

function showError(error) {
  document.querySelector("#status").textContent = `操作失败：${error.message}`;
}

function escapeHtml(value) {
  return String(value).replace(/[&<>"']/g, character => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
  })[character]);
}

async function loadConfig() {
  const config = await api("config");
  document.querySelector("#udp-enabled").checked = Boolean(config.udp_enabled);
  document.querySelector("#udp-host").value = config.udp_host || "";
  document.querySelector("#udp-port").value = config.udp_port || 5514;
}

async function refresh() {
  const status = document.querySelector("#status");
  try {
    const data = await api("tasks");
    const running = (data.tasks || []).filter(task => task.state === 2).length;
    const queued = (data.tasks || []).filter(task => task.state === 4).length;
    status.textContent = `${running} 个任务运行中，${queued} 个任务排队中`;
    document.querySelector("#tasks").innerHTML = (data.tasks || []).map(task => `
      <tr><td>${task.id}</td><td>${escapeHtml(task.name)}</td><td>${task.enabled ? "是" : "否"}</td>
      <td>${stateNames[task.state] || `未知（${task.state}）`}${task.active_step ? `（步骤 ${task.active_step}/${task.step_count}）` : ""}</td><td>${task.runs}</td>
      <td>${task.state === 2 ?
        `<button data-cancel="${task.id}">取消</button>` :
        `<button data-run="${task.id}">运行</button>`}
      <button data-edit="${task.id}">编辑</button>
      <button data-enable="${task.id}" data-value="${task.enabled ? 0 : 1}">${task.enabled ? "禁用" : "启用"}</button>
      <button data-log="${task.id}">日志</button>
      <button data-delete="${task.id}">删除</button></td></tr>`).join("");
  } catch (error) {
    status.textContent = `连接失败：${error.message}`;
  }
}

document.querySelector("#refresh").addEventListener("click", refresh);
document.querySelector("#new-task").addEventListener("click", () => {
  document.querySelector("#task-form").reset();
  document.querySelector("#task-interval").value = 60000;
  document.querySelector("#task-timeout").value = 30000;
  document.querySelector("#task-workdir").value = "/tmp";
});
document.querySelector("#task-form").addEventListener("submit", async event => {
  event.preventDefault();
  try {
    const result = requireSuccess(await apiParams({
      action: "task-save",
      id: document.querySelector("#task-id").value,
      name: document.querySelector("#task-name").value,
      enabled: document.querySelector("#task-enabled").checked ? "1" : "0",
      schedule: document.querySelector("#task-schedule").value,
      interval: document.querySelector("#task-interval").value,
      max_runs: document.querySelector("#task-max-runs").value,
      timeout: document.querySelector("#task-timeout").value,
      retry: document.querySelector("#task-retry").value,
      workdir: document.querySelector("#task-workdir").value,
      steps: document.querySelector("#task-steps").value,
      always_steps: document.querySelector("#task-always-steps").value
    }));
    document.querySelector("#task-result").textContent = result.result;
    refresh();
  } catch (error) {
    document.querySelector("#task-result").textContent = error.message;
  }
});
document.querySelector("#udp-form").addEventListener("submit", async event => {
  event.preventDefault();
  const resultBox = document.querySelector("#udp-result");
  try {
    const parameters = new URLSearchParams({
      action: "config-save",
      enabled: document.querySelector("#udp-enabled").checked ? "1" : "0",
      host: document.querySelector("#udp-host").value,
      port: document.querySelector("#udp-port").value
    });
    const response = await fetch(`/cgi-bin/tsched-api?${parameters}`);
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const result = requireSuccess(await response.json());
    resultBox.textContent = result.result;
  } catch (error) {
    resultBox.textContent = error.message;
  }
});
document.addEventListener("click", async event => {
  try {
    if (event.target.dataset.run) {
      requireSuccess(await api("run", event.target.dataset.run));
      refresh();
    } else if (event.target.dataset.cancel) {
      requireSuccess(await api("cancel", event.target.dataset.cancel));
      refresh();
    } else if (event.target.dataset.edit) {
      const task = await api("task", event.target.dataset.edit);
      document.querySelector("#task-id").value = task.id;
      document.querySelector("#task-name").value = task.name;
      document.querySelector("#task-enabled").checked = Boolean(task.enabled);
      document.querySelector("#task-schedule").value = task.schedule;
      document.querySelector("#task-interval").value = task.interval;
      document.querySelector("#task-max-runs").value = task.max_runs;
      document.querySelector("#task-timeout").value = task.timeout;
      document.querySelector("#task-retry").value = task.retry;
      document.querySelector("#task-workdir").value = task.workdir;
      document.querySelector("#task-steps").value = (task.steps || []).join("\n");
      document.querySelector("#task-always-steps").value =
        (task.always_steps || []).join("\n");
      document.querySelector("#task-result").textContent = "";
    } else if (event.target.dataset.enable) {
      requireSuccess(await apiParams({action: "task-enable",
        id: event.target.dataset.enable, enabled: event.target.dataset.value}));
      refresh();
    } else if (event.target.dataset.log) {
      const result = requireSuccess(await api("log", event.target.dataset.log));
      document.querySelector("#task-log").textContent = result.log;
    } else if (event.target.dataset.delete) {
      if (confirm("确认删除该任务？")) {
        requireSuccess(await apiParams({action: "task-delete",
          id: event.target.dataset.delete}));
        refresh();
      }
    }
  } catch (error) {
    showError(error);
  }
});
refresh();
loadConfig().catch(error => {
  document.querySelector("#udp-result").textContent = error.message;
});
setInterval(refresh, 5000);
