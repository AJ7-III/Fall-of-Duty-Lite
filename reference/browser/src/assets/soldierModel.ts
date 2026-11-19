import { assetUrl } from "./paths";

export interface SoldierDownload {
  abort: AbortController;
  bytes: Promise<Uint8Array>;
}

// Static hosts can serve the gzip as-is or decode it before delivery.
export async function fetchSoldierBytes(signal: AbortSignal): Promise<Uint8Array> {
  const response = await fetch(assetUrl("models/soldier.glb.gz"), { signal });
  if (!response.ok) throw new Error(`Unable to load the soldier model (HTTP ${response.status})`);
  const view = new Uint8Array(await response.arrayBuffer());
  if (view[0] !== 0x1f || view[1] !== 0x8b) return view;
  if (typeof DecompressionStream !== "function") {
    throw new Error("This browser cannot unpack the soldier model. Update Chrome, Edge, Firefox or Safari.");
  }
  const stream = new Blob([view]).stream().pipeThrough(new DecompressionStream("gzip"));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}
