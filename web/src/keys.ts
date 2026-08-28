export type CatalogAction = { label: string; group: string; page: number; usage: number };

const keyboard = (label: string, usage: number, group = "Keyboard"): CatalogAction => ({
  label,
  group,
  page: 0x07,
  usage,
});
const consumer = (label: string, usage: number): CatalogAction => ({
  label,
  group: "Media",
  page: 0x0c,
  usage,
});

export const KEY_CATALOG: CatalogAction[] = [
  ...Array.from({ length: 26 }, (_, index) => keyboard(String.fromCharCode(65 + index), 0x04 + index)),
  ..."1234567890".split("").map((label, index) => keyboard(label, 0x1e + index)),
  keyboard("Enter", 0x28, "Editing"), keyboard("Escape", 0x29, "Editing"),
  keyboard("Backspace", 0x2a, "Editing"), keyboard("Tab", 0x2b, "Editing"),
  keyboard("Space", 0x2c, "Editing"), keyboard("- / _", 0x2d, "Symbols"),
  keyboard("= / +", 0x2e, "Symbols"), keyboard("[ / {", 0x2f, "Symbols"),
  keyboard("] / }", 0x30, "Symbols"), keyboard("\\ / |", 0x31, "Symbols"),
  keyboard("; / :", 0x33, "Symbols"), keyboard("' / quote", 0x34, "Symbols"),
  keyboard("` / ~", 0x35, "Symbols"), keyboard(", / <", 0x36, "Symbols"),
  keyboard(". / >", 0x37, "Symbols"), keyboard("/ / ?", 0x38, "Symbols"),
  ...Array.from({ length: 12 }, (_, index) => keyboard(`F${index + 1}`, 0x3a + index, "Function")),
  keyboard("Print Screen", 0x46, "Navigation"), keyboard("Insert", 0x49, "Navigation"),
  keyboard("Home", 0x4a, "Navigation"), keyboard("Page Up", 0x4b, "Navigation"),
  keyboard("Delete", 0x4c, "Navigation"), keyboard("End", 0x4d, "Navigation"),
  keyboard("Page Down", 0x4e, "Navigation"), keyboard("Right Arrow", 0x4f, "Navigation"),
  keyboard("Left Arrow", 0x50, "Navigation"), keyboard("Down Arrow", 0x51, "Navigation"),
  keyboard("Up Arrow", 0x52, "Navigation"),
  ...Array.from({ length: 9 }, (_, index) => keyboard(`Numpad ${index + 1}`, 0x59 + index, "Numpad")),
  keyboard("Numpad 0", 0x62, "Numpad"), keyboard("Numpad .", 0x63, "Numpad"),
  consumer("Mute", 0xe2), consumer("Volume Up", 0xe9), consumer("Volume Down", 0xea),
  consumer("Play / Pause", 0xcd), consumer("Next Track", 0xb5), consumer("Previous Track", 0xb6),
  consumer("Stop", 0xb7),
];

export const MODIFIERS = [
  { label: "Ctrl", bit: 0x01 },
  { label: "Shift", bit: 0x02 },
  { label: "Alt", bit: 0x04 },
  { label: "GUI", bit: 0x08 },
] as const;

export function actionId(page: number, usage: number): string {
  return `${page}:${usage}`;
}

export function findAction(page: number, usage: number): CatalogAction {
  return KEY_CATALOG.find((action) => action.page === page && action.usage === usage) ?? KEY_CATALOG[0]!;
}
