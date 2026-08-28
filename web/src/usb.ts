import { CONFIG_SIZE, READ_CHUNK_SIZE, decodeConfig, encodeConfig, type Config } from "./protocol";

const SYNC0 = 0x5a;
const SYNC1 = 0x4d;

const COMMAND_BEGIN = 1;
const COMMAND_CHUNK = 2;
const COMMAND_COMMIT = 3;
const COMMAND_RESET = 4;

const TYPE_STATUS = 0x81;
const TYPE_CONFIG_CHUNK = 0x82;
const TYPE_REQUEST_CHUNK = 0x10;
const TYPE_REQUEST_STATUS = 0x11;

const STATUS_SAVED = 2;
const DEFAULT_TIMEOUT_MS = 3000;
const CONNECT_TIMEOUT_MS = 5000;

function checksum(bytes: Uint8Array): number {
  let sum = 0;
  for (const b of bytes) sum ^= b;
  return sum;
}

function buildFrame(type: number, payload: Uint8Array): Uint8Array {
  if (payload.length > 0xffff) {
    throw new Error("USB frame payload is too large.");
  }

  const frame = new Uint8Array(6 + payload.length);
  frame[0] = SYNC0;
  frame[1] = SYNC1;
  frame[2] = type;
  frame[3] = payload.length & 0xff;
  frame[4] = (payload.length >> 8) & 0xff;
  frame.set(payload, 5);
  frame[frame.length - 1] = checksum(frame.subarray(0, frame.length - 1));
  return frame;
}

export class MacroZUsbDevice {
  private port?: SerialPort;
  private reader?: ReadableStreamDefaultReader<Uint8Array>;
  private writer?: WritableStreamDefaultWriter<Uint8Array>;
  private rxBuffer = new Uint8Array(0);
  private pumpTask?: Promise<void>;
  private disconnectNotified = false;

  onDisconnect?: () => void;

  async connect(): Promise<Config> {
    if (!("serial" in navigator)) {
      throw new Error("Web Serial is not supported by this browser.");
    }

    try {
      this.disconnectNotified = false;
      this.port = await navigator.serial.requestPort();
      await this.port.open({ baudRate: 115200, dataBits: 8, stopBits: 1, parity: "none", flowControl: "none" });

      // Chrome/Edge do not guarantee that DTR is asserted for a newly opened
      // port. CDC ACM applications should explicitly assert it before sending.
      if (this.port.setSignals) {
        await this.port.setSignals({ dataTerminalReady: true, requestToSend: false });
      }

      if (!this.port.readable || !this.port.writable) {
        throw new Error("The selected USB serial port is not readable/writable.");
      }

      this.writer = this.port.writable.getWriter();
      this.reader = this.port.readable.getReader();
      this.pumpTask = this.pump();

      // Give the CDC ACM interface a short moment to finish enumeration and
      // process the host's DTR state before the first request.
      await new Promise((resolve) => setTimeout(resolve, 100));

      // A status request is a connection handshake. If this succeeds, the
      // browser can talk to the MacroZ firmware, rather than merely seeing it.
      const status = await this.requestStatus();
      if (status.length < 4) {
        throw new Error("Invalid status response from the macropad.");
      }

      return await this.read();
    } catch (error) {
      await this.close(false);
      throw error;
    }
  }

  private appendRx(value: Uint8Array): void {
    const merged = new Uint8Array(this.rxBuffer.length + value.length);
    merged.set(this.rxBuffer, 0);
    merged.set(value, this.rxBuffer.length);
    this.rxBuffer = merged;
  }

  private async pump(): Promise<void> {
    if (!this.reader) return;

    try {
      for (;;) {
        const { value, done } = await this.reader.read();
        if (done) break;
        if (value?.length) this.appendRx(value);
      }
    } catch (error) {
      if (!this.disconnectNotified) {
        this.disconnectNotified = true;
        this.onDisconnect?.();
      }
      throw error;
    }
  }

  private tryParseFrame(): { type: number; payload: Uint8Array } | null {
    for (;;) {
      if (this.rxBuffer.length < 6) return null;

      if (this.rxBuffer[0] !== SYNC0 || this.rxBuffer[1] !== SYNC1) {
        this.rxBuffer = this.rxBuffer.slice(1);
        continue;
      }

      const payloadLen = this.rxBuffer[3] | (this.rxBuffer[4] << 8);
      const total = 6 + payloadLen;

      if (this.rxBuffer.length < total) return null;

      const frame = this.rxBuffer.slice(0, total);
      const expected = checksum(frame.subarray(0, total - 1));
      if (frame[total - 1] !== expected) {
        // Drop one byte and search for the next MZ sync sequence.
        this.rxBuffer = this.rxBuffer.slice(1);
        continue;
      }

      this.rxBuffer = this.rxBuffer.slice(total);
      return {
        type: frame[2],
        payload: frame.slice(5, total - 1),
      };
    }
  }

  private async waitForFrame(type: number, timeoutMs = DEFAULT_TIMEOUT_MS): Promise<Uint8Array> {
    const deadline = Date.now() + timeoutMs;

    for (;;) {
      let frame: { type: number; payload: Uint8Array } | null;
      while ((frame = this.tryParseFrame()) !== null) {
        if (frame.type === type) return frame.payload;
      }

      if (Date.now() >= deadline) {
        throw new Error(`Timed out waiting for pad response (type 0x${type.toString(16)}).`);
      }

      await new Promise((resolve) => setTimeout(resolve, 5));
    }
  }

  private async writeFrame(type: number, payload = new Uint8Array(0)): Promise<void> {
    if (!this.writer) throw new Error("Pad is not connected.");
    await this.writer.write(buildFrame(type, payload));
  }

  private async requestStatus(): Promise<Uint8Array> {
    await this.writeFrame(TYPE_REQUEST_STATUS);
    return this.waitForFrame(TYPE_STATUS, CONNECT_TIMEOUT_MS);
  }

  private async sendCommand(commandByte: number, extra = new Uint8Array(0)): Promise<Uint8Array> {
    await this.writeFrame(commandByte, extra);
    return this.waitForFrame(TYPE_STATUS);
  }

  async read(): Promise<Config> {
    const bytes = new Uint8Array(CONFIG_SIZE);

    for (let offset = 0; offset < bytes.length; offset += READ_CHUNK_SIZE) {
      const offsetPayload = new Uint8Array(2);
      new DataView(offsetPayload.buffer).setUint16(0, offset, true);

      await this.writeFrame(TYPE_REQUEST_CHUNK, offsetPayload);
      const chunk = await this.waitForFrame(TYPE_CONFIG_CHUNK);

      const expected = Math.min(READ_CHUNK_SIZE, bytes.length - offset);
      if (chunk.length !== expected) {
        throw new Error(`Expected ${expected} bytes at configuration offset ${offset}, received ${chunk.length}.`);
      }

      bytes.set(chunk, offset);
    }

    return decodeConfig(new DataView(bytes.buffer));
  }

  async save(config: Config): Promise<void> {
    const bytes = encodeConfig(config);

    await this.sendCommand(COMMAND_BEGIN);

    for (let offset = 0; offset < bytes.length; offset += 16) {
      const data = bytes.slice(offset, offset + 16);
      const extra = new Uint8Array(2 + data.length);
      new DataView(extra.buffer).setUint16(0, offset, true);
      extra.set(data, 2);
      await this.sendCommand(COMMAND_CHUNK, extra);
    }

    const status = await this.sendCommand(COMMAND_COMMIT);
    if (status[0] !== STATUS_SAVED) {
      throw new Error(`Pad rejected the update (status ${status[0]}).`);
    }
  }

  async reset(): Promise<Config> {
    await this.sendCommand(COMMAND_RESET);
    return this.read();
  }

  async close(notify = true): Promise<void> {
    this.disconnectNotified = !notify;

    try {
      if (this.port?.setSignals) {
        await this.port.setSignals({ dataTerminalReady: false, requestToSend: false }).catch(() => undefined);
      }
    } catch {
      // Ignore signal errors while closing.
    }

    try {
      await this.reader?.cancel();
    } catch {
      // Ignore reader cancellation errors.
    }

    try {
      this.reader?.releaseLock();
    } catch {
      // Ignore release errors.
    }

    try {
      this.writer?.releaseLock();
    } catch {
      // Ignore release errors.
    }

    try {
      await this.port?.close();
    } catch {
      // Ignore close errors.
    }

    this.reader = undefined;
    this.writer = undefined;
    this.port = undefined;
    this.rxBuffer = new Uint8Array(0);
    this.pumpTask = undefined;

    if (notify) {
      this.onDisconnect?.();
    }
  }
}
