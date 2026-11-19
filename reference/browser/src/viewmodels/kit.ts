import { Mesh } from "@babylonjs/core/Meshes/mesh";
import { MeshBuilder } from "@babylonjs/core/Meshes/meshBuilder";
import { Quaternion, Vector3 } from "@babylonjs/core/Maths/math.vector";
import { TransformNode } from "@babylonjs/core/Meshes/transformNode";
import type { Material, PBRMaterial, Scene } from "@babylonjs/core";
import { frameQuat } from "../anim/boneMath";
import { flatMat } from "../rendering/materials/canvas";
import { roundRing } from "../rendering/meshProfiles";
import { knurlTexture } from "./weaponTextures";

// Shared building blocks for the procedural first-person weapons: primitive
// placement, mesh merging, shaped profiles, shared weapon materials, and the
// hand-anchor contract the skinned ArmsRig grips them by.

export type Vec3Tuple = readonly [number, number, number];

// A finger's close: one number spreads a curl down the joints (0 = straight,
// 1 = fully curled), or three numbers set each joint — a trigger finger
// runs straight from the knuckle and bends sharply at the middle joint
export type FingerCurl = number | readonly [number, number, number];

// How a hand holds this weapon: where the wrist sits in weapon space, which
// way the knuckles point, which way the palm faces, and how each finger closes
export interface HandPose {
  wrist: Vec3Tuple;
  knuckles: Vec3Tuple;
  palm: Vec3Tuple;
  curl: { thumb: FingerCurl; index: FingerCurl; middle: FingerCurl; ring: FingerCurl; pinky: FingerCurl };
}

// A grip point the ArmsRig chases. ViewModelRig animates `offset` and
// `altBlend` (the second pose, e.g. the hand lifted onto a bolt handle); the
// rig resolves the final frame every frame.
export interface HandAnchor {
  node: TransformNode; // parented to the weapon root; position = rest + offset
  rest: Vector3;
  offset: Vector3;
  pose: HandPose;
  alt: HandPose | null;
  altBlend: number;
}

export interface WeaponViewModel {
  root: Mesh;
  pivots: Record<string, Mesh>; // animated sub-assemblies by name
  hands: { right: HandAnchor; left: HandAnchor };
}

export interface PrimOpts {
  rx?: number;
  ry?: number;
  rz?: number;
  scale?: Vec3Tuple;
  sx?: number;
  sy?: number;
  sz?: number;
}

export const ALONG_Z = { rx: Math.PI / 2 };

const POSE_Y = new Vector3();
const POSE_Z = new Vector3();

export function handAnchor(name: string, root: Mesh, pose: HandPose, alt: HandPose | null = null): HandAnchor {
  const node = new TransformNode(name, root.getScene());
  node.parent = root;
  const rest = new Vector3(pose.wrist[0], pose.wrist[1], pose.wrist[2]);
  node.position.copyFrom(rest);
  node.rotationQuaternion = poseFrame(pose, new Quaternion());
  return { node, rest, offset: new Vector3(), pose, alt, altBlend: 0 };
}

// Orientation of a hand pose as a quaternion: +Y = knuckle direction,
// +Z = palm normal (both in weapon space)
export function poseFrame(pose: HandPose, out: Quaternion): Quaternion {
  POSE_Y.set(pose.knuckles[0], pose.knuckles[1], pose.knuckles[2]);
  POSE_Z.set(pose.palm[0], pose.palm[1], pose.palm[2]);
  return frameQuat(POSE_Y, POSE_Z, out);
}

// Place a primitive: material, optional rotation/scaling, position, parent.
export function prim(mesh: Mesh, mat: Material | null, parent: Mesh, pos: Vector3 | Vec3Tuple | null, o?: PrimOpts): Mesh {
  if (mat) mesh.material = mat;
  if (o) {
    if (o.rx !== undefined) mesh.rotation.x = o.rx;
    if (o.ry !== undefined) mesh.rotation.y = o.ry;
    if (o.rz !== undefined) mesh.rotation.z = o.rz;
    if (o.scale) mesh.scaling.set(o.scale[0], o.scale[1], o.scale[2]);
    if (o.sx !== undefined) mesh.scaling.x = o.sx;
    if (o.sy !== undefined) mesh.scaling.y = o.sy;
    if (o.sz !== undefined) mesh.scaling.z = o.sz;
  }
  if (pos) {
    if (pos instanceof Vector3) mesh.position.copyFrom(pos);
    else mesh.position.set(pos[0], pos[1], pos[2]);
  }
  mesh.parent = parent;
  return mesh;
}

export function box(
  name: string,
  size: Vec3Tuple,
  mat: Material | null,
  parent: Mesh,
  pos: Vector3 | Vec3Tuple | null,
  o?: PrimOpts
): Mesh {
  return prim(
    MeshBuilder.CreateBox(name, { width: size[0], height: size[1], depth: size[2] }, parent.getScene()),
    mat,
    parent,
    pos,
    o
  );
}

export function cyl(
  name: string,
  opts: { h: number; d?: number; top?: number; bot?: number; tess?: number },
  mat: Material | null,
  parent: Mesh,
  pos: Vector3 | Vec3Tuple | null,
  o?: PrimOpts
): Mesh {
  return prim(
    MeshBuilder.CreateCylinder(
      name,
      { height: opts.h, diameter: opts.d, diameterTop: opts.top, diameterBottom: opts.bot, tessellation: opts.tess },
      parent.getScene()
    ),
    mat,
    parent,
    pos,
    o
  );
}

export function torus(
  name: string,
  opts: { d: number; t: number; tess: number },
  mat: Material | null,
  parent: Mesh,
  pos: Vector3 | Vec3Tuple | null,
  o?: PrimOpts
): Mesh {
  return prim(roundRing(parent.getScene(), name, opts.d, opts.t, opts.tess), mat, parent, pos, o);
}

export function sharedKnurlMat(scene: Scene): PBRMaterial {
  return flatMat(scene, "weaponKnurlMat", { albedo: [0.7, 0.7, 0.74], rough: 0.4, metal: 1, tex: knurlTexture(scene) });
}

export function sharedBrassMat(scene: Scene): PBRMaterial {
  return flatMat(scene, "weaponBrassMat", { albedo: [0.85, 0.64, 0.3], rough: 0.3, metal: 1 });
}

export function sharedRubberMat(scene: Scene): PBRMaterial {
  return flatMat(scene, "weaponRubberMat", { albedo: [0.055, 0.055, 0.06], rough: 0.9 });
}

export function sharedDarkTrimMat(scene: Scene): PBRMaterial {
  return flatMat(scene, "weaponDarkTrimMat", { albedo: [0.06, 0.06, 0.07], rough: 0.55, metal: 0.8 });
}

// Merge a weapon's static parts into one mesh per material: ~120 primitives
// (~120 draw calls per frame) collapse to ~20 with pixel-identical output.
// Animated pivot groups stay separate; their children merge within the
// pivot. Must run while the root and pivots' ancestors are still at
// identity, so baked world geometry equals root-local geometry; setParent()
// compensates each pivot's rest offset. Finally marks every mesh unpickable
// so the viewmodel never intercepts the hitscan ray.
export function mergeWeaponParts(parent: Mesh, pivots: Mesh[]): void {
  const mergeByMaterial = (meshes: Mesh[], reparent: (m: Mesh) => void): void => {
    const groups = new Map<unknown, Mesh[]>();
    for (const m of meshes) {
      if (m.getTotalVertices() === 0 || !m.material) continue;
      const list = groups.get(m.material);
      if (list) list.push(m);
      else groups.set(m.material, [m]);
    }
    for (const list of groups.values()) {
      if (list.length < 2) continue;
      const merged = Mesh.MergeMeshes(list, true, true, undefined, false, false);
      if (merged) reparent(merged);
    }
  };

  const staticParts: Mesh[] = [];
  const pivotParts = new Map<Mesh, Mesh[]>(pivots.map((p) => [p, []]));
  for (const child of parent.getChildMeshes(false)) {
    const m = child as Mesh;
    if (pivots.includes(m)) continue;
    const owner = pivots.find((p) => m.isDescendantOf(p));
    if (owner) pivotParts.get(owner)!.push(m);
    else staticParts.push(m);
  }
  mergeByMaterial(staticParts, (m) => m.setParent(parent));
  for (const [pivot, parts] of pivotParts) {
    mergeByMaterial(parts, (m) => m.setParent(pivot));
  }

  parent.isPickable = false;
  for (const child of parent.getChildMeshes()) {
    child.isPickable = false;
  }
}

function beveledSection(w: number, bottom: number, top: number, z = 0): Vector3[] {
  const bevel = Math.min(w * 0.32, (top - bottom) * 0.24);
  return [
    new Vector3(-w + bevel, bottom, z),
    new Vector3(w - bevel, bottom, z),
    new Vector3(w, bottom + bevel, z),
    new Vector3(w, top - bevel, z),
    new Vector3(w - bevel, top, z),
    new Vector3(-w + bevel, top, z),
    new Vector3(-w, top - bevel, z),
    new Vector3(-w, bottom + bevel, z),
  ];
}

export function loft(
  name: string,
  parent: Mesh,
  mat: Material,
  stations: ReadonlyArray<readonly [z: number, halfWidth: number, bottom: number, top: number]>
): Mesh {
  const pathArray = stations.map(([z, w, lo, hi]) => beveledSection(w, lo, hi, z));
  const cap = ([z, , lo, hi]: (typeof stations)[number]) => Array<Vector3>(8).fill(new Vector3(0, (lo + hi) / 2, z));
  pathArray.unshift(cap(stations[0]));
  pathArray.push(cap(stations[stations.length - 1]));
  return prim(
    MeshBuilder.CreateRibbon(name, { pathArray, closePath: true, invertUV: true }, parent.getScene()),
    mat,
    parent,
    null
  );
}

export function triggerGuard(name: string, [width, drop, length]: Vec3Tuple, mat: Material, parent: Mesh, pos: Vec3Tuple): Mesh {
  const path = [
    [0, -0.5],
    [-0.68, -0.44],
    [-0.94, -0.28],
    [-1, 0.24],
    [-0.87, 0.43],
    [-0.65, 0.5],
    [0, 0.5],
  ].map(([y, z]) => new Vector3(0, y * drop, z * length));
  const shape = beveledSection(width / 2, -0.0022, 0.0022);
  const mesh = MeshBuilder.ExtrudeShape(name, { shape, path, closeShape: true, firstNormal: Vector3.Right() }, parent.getScene());
  return prim(mesh, mat, parent, pos);
}
