import { Mesh } from "@babylonjs/core/Meshes/mesh";
import { TransformNode } from "@babylonjs/core/Meshes/transformNode";
import type { FreeCamera, Material, Scene } from "@babylonjs/core";

export function placePiece(
  m: Mesh,
  mat: Material,
  parent: TransformNode,
  x: number,
  y: number,
  z: number,
  rx = 0,
  ry = 0,
  rz = 0
): Mesh {
  m.material = mat;
  m.parent = parent;
  m.position.set(x, y, z);
  m.rotation.set(rx, ry, rz);
  m.isPickable = false;
  return m;
}

// Merge static child meshes under `root` by material (ignoring `skipSubtrees`).
// Must run while `root` is unparented at identity so baked geometry equals root-local space.
export function mergeStaticMeshesByMaterial(root: TransformNode, skipSubtrees: ReadonlyArray<TransformNode> = []): void {
  const groups = new Map<unknown, Mesh[]>();
  for (const child of root.getChildMeshes(false)) {
    const m = child as Mesh;
    if (skipSubtrees.some((s) => m === (s as unknown) || m.isDescendantOf(s))) continue;
    if (m.getTotalVertices() === 0 || !m.material) continue;
    const list = groups.get(m.material);
    if (list) list.push(m);
    else groups.set(m.material, [m]);
  }
  for (const list of groups.values()) {
    if (list.length < 2) continue;
    const merged = Mesh.MergeMeshes(list, true, true, undefined, false, false);
    if (merged) {
      merged.parent = root;
      merged.isPickable = false;
    }
  }
}

export function smoothstep(t: number): number {
  const c = Math.max(0, Math.min(1, t));
  return c * c * (3 - 2 * c);
}

export abstract class CameraDevice {
  protected root: TransformNode;
  protected phaseT = 0;

  constructor(name: string, scene: Scene) {
    this.root = new TransformNode(name, scene);
  }

  protected finalizeRig(camera: FreeCamera, rx: number, ry = 0, rz = 0, skipSubtrees: ReadonlyArray<TransformNode> = []): void {
    mergeStaticMeshesByMaterial(this.root, skipSubtrees);
    this.root.parent = camera;
    this.root.rotation.set(rx, ry, rz);
    this.root.setEnabled(false);
  }
}
