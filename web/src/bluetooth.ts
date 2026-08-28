import {
  CONFIG_UUID,
  CONTROL_UUID,
  CONFIG_SIZE,
  READ_CHUNK_SIZE,
  SERVICE_UUID,
  STATUS_UUID,
  decodeConfig,
  encodeConfig,
  type Config,
} from "./protocol";

export class MacroZDevice {
  private device?: BluetoothDevice;
  private configCharacteristic?: BluetoothRemoteGATTCharacteristic;
  private controlCharacteristic?: BluetoothRemoteGATTCharacteristic;
  private statusCharacteristic?: BluetoothRemoteGATTCharacteristic;

  onDisconnect?: () => void;

  async connect(): Promise<Config> {
    this.device = await navigator.bluetooth.requestDevice({
      filters: [{ namePrefix: "Labib Macropad" }],
      optionalServices: [SERVICE_UUID],
    });
    this.device.addEventListener("gattserverdisconnected", () => this.onDisconnect?.());
    const server = await this.device.gatt?.connect();
    if (!server) throw new Error("Bluetooth connection failed.");
    const service = await server.getPrimaryService(SERVICE_UUID);
    [this.configCharacteristic, this.controlCharacteristic, this.statusCharacteristic] =
      await Promise.all([
        service.getCharacteristic(CONFIG_UUID),
        service.getCharacteristic(CONTROL_UUID),
        service.getCharacteristic(STATUS_UUID),
      ]);
    return this.read();
  }

  async read(): Promise<Config> {
    if (!this.configCharacteristic || !this.controlCharacteristic) {
      throw new Error("Pad is not connected.");
    }

    const bytes = new Uint8Array(CONFIG_SIZE);
    for (let offset = 0; offset < bytes.length; offset += READ_CHUNK_SIZE) {
      const command = new Uint8Array(3);
      command[0] = 5;
      new DataView(command.buffer).setUint16(1, offset, true);
      await this.controlCharacteristic.writeValueWithResponse(command);
      const chunk = await this.configCharacteristic.readValue();
      const expected = Math.min(READ_CHUNK_SIZE, bytes.length - offset);
      if (chunk.byteLength !== expected) {
        throw new Error(`Expected ${expected} bytes at configuration offset ${offset}.`);
      }
      bytes.set(new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength), offset);
    }
    return decodeConfig(new DataView(bytes.buffer));
  }

  async save(config: Config): Promise<void> {
    if (!this.controlCharacteristic || !this.statusCharacteristic) {
      throw new Error("Pad is not connected.");
    }
    const bytes = encodeConfig(config);
    await this.controlCharacteristic.writeValueWithResponse(Uint8Array.of(1));
    for (let offset = 0; offset < bytes.length; offset += 16) {
      const data = bytes.slice(offset, offset + 16);
      const packet = new Uint8Array(3 + data.length);
      packet[0] = 2;
      new DataView(packet.buffer).setUint16(1, offset, true);
      packet.set(data, 3);
      await this.controlCharacteristic.writeValueWithResponse(packet);
    }
    await this.controlCharacteristic.writeValueWithResponse(Uint8Array.of(3));
    const status = await this.statusCharacteristic.readValue();
    if (status.getUint8(0) !== 2) throw new Error(`Pad rejected the update (status ${status.getUint8(0)}).`);
  }

  async reset(): Promise<Config> {
    if (!this.controlCharacteristic) throw new Error("Pad is not connected.");
    await this.controlCharacteristic.writeValueWithResponse(Uint8Array.of(4));
    return this.read();
  }
}
