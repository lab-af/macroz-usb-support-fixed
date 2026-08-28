export const PROTOCOL_VERSION = 2;
export const KEY_COUNT = 9;
export const MACRO_COUNT = 6;
export const STEP_COUNT = 255;
export const READ_CHUNK_SIZE = 256;
const HEADER_SIZE = 8;
const BINDING_SIZE = 8;
const STEP_SIZE = 6;
const MACRO_SIZE = 2 + STEP_COUNT * STEP_SIZE;
const MACRO_OFFSET = HEADER_SIZE + KEY_COUNT * BINDING_SIZE;
export const CONFIG_SIZE = MACRO_OFFSET + MACRO_COUNT * MACRO_SIZE;

export const SERVICE_UUID = "6d7f2f00-7b6b-4d9d-a862-8e6e4f4d5a10";
export const CONFIG_UUID = "6d7f2f01-7b6b-4d9d-a862-8e6e4f4d5a10";
export const CONTROL_UUID = "6d7f2f02-7b6b-4d9d-a862-8e6e4f4d5a10";
export const STATUS_UUID = "6d7f2f03-7b6b-4d9d-a862-8e6e4f4d5a10";

export type Action = {
  page: number;
  usage: number;
  modifiers: number;
};

export type Binding =
  | ({ kind: "key" } & Action)
  | { kind: "macro"; macroIndex: number };

export type MacroStep = Action & { delay: number };

export type Config = {
  bindings: Binding[];
  macros: MacroStep[][];
};

const defaultUsages = [0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70];

export function defaultConfig(): Config {
  return {
    bindings: defaultUsages.map((usage) => ({
      kind: "key" as const,
      page: 0x07,
      usage,
      modifiers: 0,
    })),
    macros: Array.from({ length: MACRO_COUNT }, () => []),
  };
}

export function encodeConfig(config: Config): Uint8Array {
  if (config.bindings.length !== KEY_COUNT || config.macros.length !== MACRO_COUNT) {
    throw new Error("Configuration has the wrong number of keys or macro slots.");
  }

  const bytes = new Uint8Array(CONFIG_SIZE);
  const view = new DataView(bytes.buffer);
  view.setUint16(0, 0x5a4d, true);
  bytes[2] = PROTOCOL_VERSION;
  bytes[3] = KEY_COUNT;
  bytes[4] = MACRO_COUNT;
  bytes[5] = STEP_COUNT;

  config.bindings.forEach((binding, index) => {
    const offset = 8 + index * 8;
    if (binding.kind === "key") {
      bytes[offset] = 0;
      bytes[offset + 1] = binding.page;
      view.setUint16(offset + 2, binding.usage, true);
      bytes[offset + 4] = binding.modifiers;
    } else {
      bytes[offset] = 1;
      bytes[offset + 5] = binding.macroIndex;
    }
  });

  config.macros.forEach((steps, macroIndex) => {
    if (steps.length > STEP_COUNT) throw new Error(`Macro ${macroIndex + 1} is too long.`);
    const macroOffset = MACRO_OFFSET + macroIndex * MACRO_SIZE;
    bytes[macroOffset] = steps.length;
    steps.forEach((step, stepIndex) => {
      const offset = macroOffset + 2 + stepIndex * STEP_SIZE;
      bytes[offset] = step.page;
      bytes[offset + 1] = step.modifiers;
      view.setUint16(offset + 2, step.usage, true);
      view.setUint16(offset + 4, Math.min(5000, Math.max(0, step.delay)), true);
    });
  });

  return bytes;
}

export function decodeConfig(value: DataView): Config {
  if (value.byteLength !== CONFIG_SIZE) throw new Error(`Expected ${CONFIG_SIZE} bytes from pad.`);
  if (
    value.getUint16(0, true) !== 0x5a4d ||
    value.getUint8(2) !== PROTOCOL_VERSION ||
    value.getUint8(3) !== KEY_COUNT ||
    value.getUint8(4) !== MACRO_COUNT ||
    value.getUint8(5) !== STEP_COUNT
  ) {
    throw new Error("The pad uses an incompatible configuration format.");
  }

  const bindings: Binding[] = Array.from({ length: KEY_COUNT }, (_, index) => {
    const offset = 8 + index * 8;
    if (value.getUint8(offset) === 1) {
      return { kind: "macro", macroIndex: value.getUint8(offset + 5) };
    }
    return {
      kind: "key",
      page: value.getUint8(offset + 1),
      usage: value.getUint16(offset + 2, true),
      modifiers: value.getUint8(offset + 4),
    };
  });

  const macros = Array.from({ length: MACRO_COUNT }, (_, macroIndex) => {
    const macroOffset = MACRO_OFFSET + macroIndex * MACRO_SIZE;
    const length = Math.min(value.getUint8(macroOffset), STEP_COUNT);
    return Array.from({ length }, (_, stepIndex) => {
      const offset = macroOffset + 2 + stepIndex * STEP_SIZE;
      return {
        page: value.getUint8(offset),
        modifiers: value.getUint8(offset + 1),
        usage: value.getUint16(offset + 2, true),
        delay: value.getUint16(offset + 4, true),
      };
    });
  });

  return { bindings, macros };
}
