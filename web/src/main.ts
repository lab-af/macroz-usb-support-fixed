import "./style.css";
import { MacroZDevice } from "./bluetooth";
import { MacroZUsbDevice } from "./usb";

/** Either transport exposes this same shape, so the rest of the app doesn't
 * need to care which one is active. */
type MacroZTransport = MacroZDevice | MacroZUsbDevice;
import { KEY_CATALOG, MODIFIERS, actionId, findAction } from "./keys";
import { MACRO_COUNT, STEP_COUNT, defaultConfig, type Action, type Config } from "./protocol";

const required = <T extends Element>(selector: string): T => {
  const element = document.querySelector<T>(selector);
  if (!element) throw new Error(`Missing element: ${selector}`);
  return element;
};

const pad = required<HTMLDivElement>("#pad");
const heading = required<HTMLHeadingElement>("#editor-heading");
const selectionLabel = required<HTMLSpanElement>("#selection-label");
const keyEditor = required<HTMLDivElement>("#key-editor");
const macroEditor = required<HTMLDivElement>("#macro-editor");
const modeKey = required<HTMLButtonElement>("#mode-key");
const modeMacro = required<HTMLButtonElement>("#mode-macro");
const keyAction = required<HTMLSelectElement>("#key-action");
const keyModifiers = required<HTMLDivElement>("#key-modifiers");
const macroSlot = required<HTMLSelectElement>("#macro-slot");
const macroSteps = required<HTMLDivElement>("#macro-steps");
const addStep = required<HTMLButtonElement>("#add-step");
const connectButton = required<HTMLButtonElement>("#connect");
const connectUsbButton = required<HTMLButtonElement>("#connect-usb");
const saveButton = required<HTMLButtonElement>("#save");
const resetButton = required<HTMLButtonElement>("#reset");
const message = required<HTMLDivElement>("#message");
const connectionLabel = required<HTMLSpanElement>("#connection-label");
const connectionLight = required<HTMLSpanElement>("#connection-light");

let config: Config = defaultConfig();
let selectedKey = 0;
let connected = false;
let device: MacroZTransport = new MacroZDevice();

function attachDisconnectHandler(target: MacroZTransport): void {
  target.onDisconnect = () => {
    setConnected(false);
    setMessage("Pad disconnected. Your current edits are still here.", true);
  };
}
attachDisconnectHandler(device);

function setMessage(text: string, error = false): void {
  message.textContent = text;
  message.classList.toggle("error", error);
}

function actionOptions(selected: Action): HTMLOptGroupElement[] {
  const groups = new Map<string, HTMLOptGroupElement>();
  for (const action of KEY_CATALOG) {
    let group = groups.get(action.group);
    if (!group) {
      group = document.createElement("optgroup");
      group.label = action.group;
      groups.set(action.group, group);
    }
    const option = document.createElement("option");
    option.value = actionId(action.page, action.usage);
    option.textContent = action.label;
    option.selected = action.page === selected.page && action.usage === selected.usage;
    group.append(option);
  }
  return [...groups.values()];
}

function selectedBinding() {
  return config.bindings[selectedKey]!;
}

function bindingLabel(index: number): string {
  const binding = config.bindings[index]!;
  if (binding.kind === "macro") return `M${binding.macroIndex + 1}`;
  const action = findAction(binding.page, binding.usage);
  const mods = MODIFIERS.filter((modifier) => binding.modifiers & modifier.bit).map((modifier) => modifier.label[0]);
  return mods.length ? `${mods.join("")}+${action.label}` : action.label;
}

function renderPad(): void {
  pad.replaceChildren();
  config.bindings.forEach((binding, index) => {
    const button = document.createElement("button");
    button.className = "pad-key";
    if (index === selectedKey) button.classList.add("selected");
    if (binding.kind === "macro") button.classList.add("has-macro");
    button.setAttribute("aria-label", `Key ${index + 1}: ${bindingLabel(index)}`);
    button.innerHTML = `<span class="key-index">${String(index + 1).padStart(2, "0")}</span><span class="key-label"></span>`;
    button.querySelector(".key-label")!.textContent = bindingLabel(index);
    button.addEventListener("click", () => {
      selectedKey = index;
      render();
    });
    pad.append(button);
  });
}

function renderModifiers(container: HTMLElement, modifiers: number, onChange: (value: number) => void): void {
  container.replaceChildren();
  for (const modifier of MODIFIERS) {
    const label = document.createElement("label");
    label.className = "modifier";
    const checkbox = document.createElement("input");
    checkbox.type = "checkbox";
    checkbox.checked = Boolean(modifiers & modifier.bit);
    checkbox.addEventListener("change", () => {
      const next = checkbox.checked ? modifiers | modifier.bit : modifiers & ~modifier.bit;
      onChange(next);
    });
    label.append(checkbox, document.createTextNode(modifier.label));
    container.append(label);
  }
}

function renderKeyEditor(): void {
  const binding = selectedBinding();
  if (binding.kind !== "key") return;
  keyAction.replaceChildren(...actionOptions(binding));
  renderModifiers(keyModifiers, binding.modifiers, (modifiers) => {
    config.bindings[selectedKey] = { ...binding, modifiers };
    render();
  });
}

function parseAction(value: string): Pick<Action, "page" | "usage"> {
  const [page, usage] = value.split(":").map(Number);
  return { page: page!, usage: usage! };
}

function renderMacroEditor(): void {
  const binding = selectedBinding();
  if (binding.kind !== "macro") return;
  macroSlot.replaceChildren(
    ...Array.from({ length: MACRO_COUNT }, (_, index) => {
      const option = document.createElement("option");
      option.value = String(index);
      option.textContent = `M${index + 1} · ${config.macros[index]!.length} step${config.macros[index]!.length === 1 ? "" : "s"}`;
      option.selected = binding.macroIndex === index;
      return option;
    }),
  );

  const steps = config.macros[binding.macroIndex]!;
  macroSteps.replaceChildren();
  if (!steps.length) {
    const empty = document.createElement("div");
    empty.className = "empty-macro";
    empty.textContent = "This macro is empty. Add its first action below.";
    macroSteps.append(empty);
  }

  steps.forEach((step, index) => {
    const row = document.createElement("div");
    row.className = "macro-step";
    const number = document.createElement("span");
    number.className = "step-number";
    number.textContent = String(index + 1).padStart(2, "0");
    const controls = document.createElement("div");
    controls.className = "step-controls";
    const select = document.createElement("select");
    select.className = "select step-select";
    select.append(...actionOptions(step));
    select.addEventListener("change", () => {
      Object.assign(step, parseAction(select.value));
      render();
    });
    const modifierContainer = document.createElement("div");
    modifierContainer.className = "modifier-grid mini";
    renderModifiers(modifierContainer, step.modifiers, (modifiers) => {
      step.modifiers = modifiers;
      render();
    });
    controls.append(select, modifierContainer);
    const delay = document.createElement("label");
    delay.className = "delay-input";
    const input = document.createElement("input");
    input.type = "number";
    input.min = "0";
    input.max = "5000";
    input.step = "10";
    input.value = String(step.delay);
    input.addEventListener("change", () => {
      step.delay = Math.min(5000, Math.max(0, Number(input.value) || 0));
      input.value = String(step.delay);
    });
    delay.append(input, document.createTextNode("ms"));
    const remove = document.createElement("button");
    remove.className = "remove-step";
    remove.type = "button";
    remove.setAttribute("aria-label", `Remove step ${index + 1}`);
    remove.textContent = "×";
    remove.addEventListener("click", () => {
      steps.splice(index, 1);
      render();
    });
    row.append(number, controls, delay, remove);
    macroSteps.append(row);
  });
  addStep.disabled = steps.length >= STEP_COUNT;
}

function render(): void {
  const binding = selectedBinding();
  const keyNumber = String(selectedKey + 1).padStart(2, "0");
  heading.textContent = `Key ${keyNumber}`;
  selectionLabel.textContent = `Key ${keyNumber} selected`;
  const isMacro = binding.kind === "macro";
  modeKey.classList.toggle("active", !isMacro);
  modeMacro.classList.toggle("active", isMacro);
  keyEditor.classList.toggle("hidden", isMacro);
  macroEditor.classList.toggle("hidden", !isMacro);
  renderPad();
  if (isMacro) renderMacroEditor(); else renderKeyEditor();
}

function setConnected(value: boolean): void {
  connected = value;
  saveButton.disabled = !value;
  resetButton.disabled = !value;
  connectionLight.classList.toggle("online", value);
  connectionLabel.textContent = value ? "Labib Macropad connected" : "Offline editor";
  connectButton.textContent = value ? "Reconnect" : "Connect pad";
}

modeKey.addEventListener("click", () => {
  const current = selectedBinding();
  if (current.kind === "macro") {
    config.bindings[selectedKey] = { kind: "key", page: 0x07, usage: 0x04, modifiers: 0 };
    render();
  }
});

modeMacro.addEventListener("click", () => {
  if (selectedBinding().kind !== "macro") {
    config.bindings[selectedKey] = { kind: "macro", macroIndex: 0 };
    render();
  }
});

keyAction.addEventListener("change", () => {
  const binding = selectedBinding();
  if (binding.kind === "key") config.bindings[selectedKey] = { ...binding, ...parseAction(keyAction.value) };
  render();
});

macroSlot.addEventListener("change", () => {
  config.bindings[selectedKey] = { kind: "macro", macroIndex: Number(macroSlot.value) };
  render();
});

addStep.addEventListener("click", () => {
  const binding = selectedBinding();
  if (binding.kind !== "macro") return;
  const steps = config.macros[binding.macroIndex]!;
  if (steps.length < STEP_COUNT) steps.push({ page: 0x07, usage: 0x04, modifiers: 0, delay: 50 });
  render();
});

connectButton.addEventListener("click", async () => {
  connectButton.disabled = true;
  connectUsbButton.disabled = true;
  setMessage("Waiting for a Bluetooth device…");
  try {
    device = new MacroZDevice();
    attachDisconnectHandler(device);
    config = await device.connect();
    setConnected(true);
    setMessage("Configuration read from the pad.");
    render();
  } catch (error) {
    setConnected(false);
    setMessage(error instanceof Error ? error.message : "Could not connect.", true);
  } finally {
    connectButton.disabled = false;
    connectUsbButton.disabled = false;
  }
});

connectUsbButton.addEventListener("click", async () => {
  connectButton.disabled = true;
  connectUsbButton.disabled = true;
  setMessage("Waiting for a USB device…");
  try {
    device = new MacroZUsbDevice();
    attachDisconnectHandler(device);
    config = await device.connect();
    setConnected(true);
    setMessage("Configuration read from the pad.");
    render();
  } catch (error) {
    setConnected(false);
    setMessage(error instanceof Error ? error.message : "Could not connect.", true);
  } finally {
    connectButton.disabled = false;
    connectUsbButton.disabled = false;
  }
});

saveButton.addEventListener("click", async () => {
  saveButton.disabled = true;
  setMessage("Writing configuration…");
  try {
    await device.save(config);
    setMessage("Saved. The new layout is active now.");
  } catch (error) {
    setMessage(error instanceof Error ? error.message : "Save failed.", true);
  } finally {
    saveButton.disabled = !connected;
  }
});

resetButton.addEventListener("click", async () => {
  if (!window.confirm("Restore the pad's default numpad layout and clear assignments?")) return;
  try {
    config = await device.reset();
    setMessage("Factory layout restored.");
    render();
  } catch (error) {
    setMessage(error instanceof Error ? error.message : "Reset failed.", true);
  }
});

const isLinux = navigator.userAgent.includes("Linux");

if (!("serial" in navigator)) {
  connectUsbButton.disabled = true;
  connectUsbButton.title = "Web Serial is unavailable in this browser.";
}

if (!("bluetooth" in navigator)) {
  const unsupported = required<HTMLDivElement>("#unsupported");
  if (!window.isSecureContext) {
    unsupported.textContent = "Web Bluetooth requires HTTPS or localhost.";
  } else if (isLinux) {
    unsupported.textContent =
      "Web Bluetooth is disabled in this Linux browser. In Chrome, enable Experimental Web Platform features at chrome://flags/#enable-experimental-web-platform-features, relaunch Chrome, and reload this page.";
  }
  unsupported.classList.remove("hidden");
  connectButton.disabled = true;
}

render();
