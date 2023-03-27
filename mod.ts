import ModuleFactory from "./dist/main.js";

const createMalloc = (handler) => {
  const cleanSet = [];
  return [
    (arrOrString) => {
      if (!arrOrString) return [null, 0];
      const inputArray =
        typeof arrOrString === "string"
          ? new TextEncoder().encode(arrOrString + "\0")
          : arrOrString;
      const size = inputArray.length;
      const ptr = handler._malloc(size);
      handler.HEAPU8.set(inputArray, ptr);
      cleanSet.push(() => handler._free(ptr));
      return [ptr, size];
    },
    () => cleanSet.forEach((fn) => fn()),
  ];
};

export const probe = async (
  avArray: Uint8Array,
  sourceContainer: string,
  verbose = 0
) => {
  const handler = await ModuleFactory({ INITIAL_MEMORY: 128 * 1024 * 1024 });
  const [malloc, free] = createMalloc(handler);
  const [avArrayPtr, size] = malloc(avArray);
  const [sourceContainerPtr] = malloc(sourceContainer);
  const structPtr = handler._probe(
    avArrayPtr,
    size,
    sourceContainerPtr,
    verbose
  );
  const view = new DataView(handler.HEAPU8.buffer, structPtr, 18);
  const res = {
    outPtr: view.getUint32(0, true),
    size: view.getUint32(4, true),
    sampleRate: view.getUint16(8, true),
    bitDepth: view.getUint16(10, true),
    numChannels: view.getUint16(12, true),
    duration: view.getUint16(14, true),
    frameSize: view.getUint16(16, true),
  };
  free();
  return res;
};

export const transcode = async (
  avArray: Uint8Array,
  sourceContainer: string,
  destContainer: string,
  destCodec: string,
  destContainerOptions?: Record<string, string>,
  verbose = 0,
  initialMemory: number = 128 * 1024 * 1024
) => {
  const destContainerOptionsString = destContainerOptions
    ? Object.entries(destContainerOptions)
        .map((v) => v.join(":"))
        .join(",")
    : null;
  const handler = await ModuleFactory({ INITIAL_MEMORY: initialMemory });
  const [malloc, free] = createMalloc(handler);
  const [avArrayPtr, size] = malloc(avArray);
  const [sourceContainerPtr] = malloc(sourceContainer);
  const [destContainerPtr] = malloc(destContainer);
  const [destCodecPtr] = malloc(destCodec);
  const [destContainerOptionsPtr] = malloc(destContainerOptionsString);
  const structPtr = handler._transcode(
    avArrayPtr,
    size,
    sourceContainerPtr,
    destContainerPtr,
    destCodecPtr,
    destContainerOptionsPtr,
    verbose
  );
  const view = new DataView(handler.HEAPU8.buffer, structPtr, 18);
  const res = {
    outPtr: view.getUint32(0, true),
    size: view.getUint32(4, true),
    sample_rate: view.getUint16(8, true),
    duration: view.getUint16(14, true),
  };
  const data = new Uint8Array(
    new Uint8Array(handler.HEAPU8.buffer, res.outPtr, res.size)
  );
  free();
  return { data, ...res };
};
