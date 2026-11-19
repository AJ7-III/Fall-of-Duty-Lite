import { MeshBuilder } from "@babylonjs/core/Meshes/meshBuilder";
import { Vector3 } from "@babylonjs/core/Maths/math.vector";
import type { Mesh, Scene } from "@babylonjs/core";

// Trace the cross-section from the inner back to the outer wall and around to the face.
export function revolve(
  scene: Scene,
  name: string,
  sides: number,
  profile: ReadonlyArray<readonly [radius: number, height: number]>,
  closed = false
): Mesh {
  const pathArray = Array.from({ length: sides }, (_, i) => {
    const angle = (i * Math.PI * 2) / sides;
    const x = Math.cos(angle);
    const z = Math.sin(angle);
    return profile.map(([r, y]) => new Vector3(r * x, y, r * z));
  });
  return MeshBuilder.CreateRibbon(name, { pathArray, closeArray: true, closePath: closed, invertUV: true }, scene);
}

export function roundRing(scene: Scene, name: string, diameter: number, thickness: number, sides: number): Mesh {
  const profile = Array.from({ length: 8 }, (_, i) => {
    const angle = (i * Math.PI) / 4;
    return [(diameter - thickness * Math.cos(angle)) / 2, (-thickness * Math.sin(angle)) / 2] as const;
  });
  return revolve(scene, name, sides, profile, true);
}
