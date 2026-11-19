import { LoadAssetContainerAsync } from "@babylonjs/core/Loading/sceneLoader";
import "@babylonjs/loaders/glTF/2.0/glTFLoader";
import { Matrix, Quaternion, Vector3 } from "@babylonjs/core/Maths/math.vector";
import { TransformNode } from "@babylonjs/core/Meshes/transformNode";
import type { AssetContainer, InstantiatedEntries, Scene } from "@babylonjs/core";
import { fetchSoldierBytes } from "../assets/soldierModel";
import type { SoldierDownload } from "../assets/soldierModel";

interface Entry extends SoldierDownload {
  container: AssetContainer | null;
  waiters: Array<(container: AssetContainer) => void>;
  ready: Promise<void>;
}

export interface AlignedSoldier {
  inst: InstantiatedEntries;
  instRoot: TransformNode;
  align: TransformNode;
  joints: Record<string, TransformNode>;
  j: (key: string) => TransformNode;
  computeAll: () => void;
  back: Vector3;
  rootInv: Matrix;
}

const TMP_V = new Vector3();
const TMP_D = new Vector3();
const TMP_Q = new Quaternion();

// Each load gets its own view over the shared bytes
export function loadSoldierContainer(bytes: Uint8Array, scene: Scene): Promise<AssetContainer> {
  return LoadAssetContainerAsync(new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength), scene, {
    pluginExtension: ".glb",
  });
}

// Weak scene keys prevent an abandoned game from retaining its model. Preview
// scenes can coexist with the main scene without replacing its pending load.
const entries = new WeakMap<Scene, Entry>();
// The live game's download, so the start-screen preview reuses its bytes
let current: Entry | null = null;

export function soldierModelBytes(): Promise<Uint8Array> {
  return current ? current.bytes : fetchSoldierBytes(new AbortController().signal);
}

export function preloadSoldierModel(scene: Scene, download?: SoldierDownload): Promise<void> {
  const existing = entries.get(scene);
  if (existing) return existing.ready;
  const abort = download?.abort ?? new AbortController();
  const bytes = download?.bytes ?? fetchSoldierBytes(abort.signal);
  const entry: Entry = { container: null, waiters: [], ready: Promise.resolve(), bytes, abort };
  entries.set(scene, entry);
  current = entry;
  scene.onDisposeObservable.addOnce(() => {
    abort.abort(); // a disposed game (retry, timeout) stops its download
    if (current === entry) current = null;
    entries.delete(scene);
    entry.waiters.length = 0;
    entry.container?.dispose();
    entry.container = null;
  });
  entry.ready = entry.bytes
    .then((bytes) => (scene.isDisposed ? null : loadSoldierContainer(bytes, scene)))
    .then((container) => {
      if (!container) return;
      if (scene.isDisposed) {
        container.dispose();
        return;
      }
      entry.container = container;
      for (const waiter of entry.waiters) waiter(container);
      entry.waiters.length = 0;
    });
  return entry.ready;
}

export function whenSoldierModelReady(scene: Scene, callback: (container: AssetContainer) => void): void {
  const entry = entries.get(scene);
  if (!entry || scene.isDisposed) return;
  if (entry.container) callback(entry.container);
  else entry.waiters.push(callback);
}

export function instantiateAlignedSoldier(
  container: AssetContainer,
  scene: Scene,
  parentRoot: TransformNode,
  prefix: string,
  targetHeight = 1.85
): AlignedSoldier {
  const inst = container.instantiateModelsToScene((n) => `${prefix}_${n}`, false, { doNotInstantiate: true });
  const instRoot = inst.rootNodes[0] as TransformNode;
  const align = new TransformNode(`${prefix}_align`, scene);
  align.parent = parentRoot;
  instRoot.parent = align;

  const computeAll = (): void => {
    parentRoot.computeWorldMatrix(true);
    align.computeWorldMatrix(true);
    for (const n of align.getDescendants(false)) (n as TransformNode).computeWorldMatrix(true);
  };
  computeAll();

  // Joint registry by short Mixamo name
  const joints: Record<string, TransformNode> = {};
  for (const n of align.getDescendants(false)) {
    const i = n.name.indexOf("mixamorig:");
    if (i >= 0) joints[n.name.slice(i + "mixamorig:".length)] = n as TransformNode;
  }
  const j = (key: string): TransformNode => {
    const node = joints[key];
    if (!node) throw new Error(`${prefix} rig missing joint ${key}`);
    return node;
  };

  // Face the character down parentRoot's +Z: the toes tell us where the
  // model actually looks, whatever the exporter and the RH->LH root did.
  TMP_V.copyFrom(j("LeftToe_End").getAbsolutePosition())
    .addInPlace(j("RightToe_End").getAbsolutePosition())
    .subtractInPlace(j("LeftToeBase").getAbsolutePosition())
    .subtractInPlace(j("RightToeBase").getAbsolutePosition());
  TMP_Q.copyFrom(parentRoot.absoluteRotationQuaternion).invertInPlace();
  TMP_V.rotateByQuaternionToRef(TMP_Q, TMP_V);
  align.rotation.y = -Math.atan2(TMP_V.x, TMP_V.z);

  // Normalize height in parentRoot-local space so a tilted root never mismeasures world-Y span.
  computeAll();
  const rootInv = new Matrix();
  parentRoot.computeWorldMatrix(true).invertToRef(rootInv);
  const headTopLocalY = Vector3.TransformCoordinatesToRef(j("HeadTop_End").getAbsolutePosition(), rootInv, TMP_V).y;
  const baseLocalY = Vector3.TransformCoordinatesToRef(align.getAbsolutePosition(), rootInv, TMP_D).y;
  const h = Math.max(0.1, headTopLocalY - baseLocalY);
  align.scaling.setAll((targetHeight / h) * align.scaling.x);
  computeAll();

  const back = Vector3.Forward().rotateByQuaternionToRef(parentRoot.absoluteRotationQuaternion, new Vector3()).scaleInPlace(-1);
  return { inst, instRoot, align, joints, j, computeAll, back, rootInv };
}
