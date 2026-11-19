import { Scene } from "@babylonjs/core/scene";
import { Mesh } from "@babylonjs/core/Meshes/mesh";
import { MeshBuilder } from "@babylonjs/core/Meshes/meshBuilder";
// Thin instances live in a side-effect module since Babylon 9: without this
// import Mesh.thinInstanceSetBuffer is a silent no-op
import "@babylonjs/core/Meshes/thinInstanceMesh";
import { Vector3, Vector4, Matrix, Quaternion } from "@babylonjs/core/Maths/math.vector";
import { Color3, Color4 } from "@babylonjs/core/Maths/math.color";
import { HemisphericLight } from "@babylonjs/core/Lights/hemisphericLight";
import { DirectionalLight } from "@babylonjs/core/Lights/directionalLight";
import { ShadowGenerator } from "@babylonjs/core/Lights/Shadows/shadowGenerator";
import { PBRMaterial } from "@babylonjs/core/Materials/PBR/pbrMaterial";
import { RenderTargetTexture } from "@babylonjs/core/Materials/Textures/renderTargetTexture";
import { ParticleSystem } from "@babylonjs/core/Particles/particleSystem";
import type { Material } from "@babylonjs/core";
import type { WorldMaterials } from "./materials/WorldMaterials";
import { flatMat } from "../rendering/materials/canvas";
import { yardEnvironment } from "../rendering/materials/environment";
import { dynamicShadowCasters, onDynamicCasterAdded } from "../rendering/dynamicShadows";
import { CarWreck } from "./CarWreck";
import { TruckWreck } from "./TruckWreck";
import { PlayerController } from "../player/PlayerController";
import { Settings } from "../ui/Settings";
import type { GraphicsQuality } from "../ui/Settings";

// Ship Box is a tight container yard, scaled off the overhead reference: a 32m
// walled square (x/z -16..16) where the four center blocks leave only
// ~2.5-3m lanes between them. Set on rain-soaked grass under an overcast
// sky, with stone walkways tracing the lanes and the wall footing.
export class ShipBoxMap {
  private scene: Scene;
  private materials: WorldMaterials;
  private shadowGen: ShadowGenerator | null = null;
  private shadowMap: RenderTargetTexture | null = null;
  private shadowsReady = false;
  private dynamicShadows = false;

  // Static props collected during build, then merged into one mesh per
  // material before play starts. Yard props cast shadows; OOB backdrop
  // dressing stays out of the shadow caster pass.
  private casterParts = new Map<Material, Mesh[]>();
  private nonCasterParts = new Map<Material, Mesh[]>();
  private wrecks: Array<{ resetGlass(): void }> = [];

  // The rain volume tracks the player so a modest particle budget always
  // fills the sky overhead (update() copies the camera x/z in here)
  private rainAnchor = new Vector3(0, 8, 0);
  private rain: ParticleSystem | null = null;

  constructor(scene: Scene, materials: WorldMaterials) {
    this.scene = scene;
    this.materials = materials;

    PlayerController.clearObstacles();

    this.createLighting();
    this.createEnvironment();
    this.createCenterBlocks();
    this.createEdgeStructures();
    this.createCornerClutter();
    this.createOutOfBounds();
    this.mergeStaticParts();
    this.createLongGrass();
    this.createRain();
    this.freezeShadows();
  }

  private createLighting(): void {
    this.scene.environmentTexture = yardEnvironment(this.scene);
    this.scene.environmentIntensity = 1.0;
    const ambientLight = new HemisphericLight("ambientLight", new Vector3(0, 1, 0), this.scene);
    ambientLight.intensity = 0.82;
    ambientLight.diffuse = new Color3(0.78, 0.82, 0.88);
    ambientLight.groundColor = new Color3(0.38, 0.41, 0.44);

    const sunLight = new DirectionalLight("sunLight", new Vector3(-0.45, -0.9, 0.35), this.scene);
    sunLight.position = new Vector3(25, 45, -25);
    sunLight.intensity = 2.2;
    sunLight.diffuse = new Color3(0.86, 0.87, 0.9);
    sunLight.specular = new Color3(0.7, 0.72, 0.76);
    // Fit frustum to the playable yard casters on frame 1 instead of
    // recomputing light bounds every frame.
    sunLight.autoUpdateExtends = false;
    sunLight.autoCalcShadowZBounds = true;
    sunLight.shadowOrthoScale = 0.05;

    this.shadowGen = new ShadowGenerator(2048, sunLight);
    this.shadowGen.usePercentageCloserFiltering = true;
    this.shadowGen.bias = 0.0006;
    this.shadowGen.setDarkness(0.45); // diffuse skylight lifts the shadows

    this.scene.fogMode = Scene.FOGMODE_LINEAR;
    this.scene.fogStart = 32;
    this.scene.fogEnd = 140;
    this.scene.fogColor = new Color3(0.5, 0.53, 0.56);

    const sky = MeshBuilder.CreateSphere("skyDome", { diameter: 600, segments: 32, sideOrientation: 1 }, this.scene);
    sky.material = this.materials.createSkyMaterial();
    sky.isPickable = false;
    sky.freezeWorldMatrix();
  }

  private static tiledUV(u: number, v: number): Vector4[] {
    const face = new Vector4(0, 0, u, v);
    return [face, face, face, face, face, face];
  }

  private createEnvironment(): void {
    // Rain-soaked lawn: the yard plus the out-of-bounds surround
    const floor = MeshBuilder.CreateGround("floor", { width: 100, height: 100 }, this.scene);
    floor.material = this.materials.createGrassMaterial(12, 12);
    floor.receiveShadows = true;
    floor.freezeWorldMatrix();

    // Stone walkways: a cross tracing the center lanes out to the tunnel
    // mouths, plus a footing path ringing the inside of the walls.
    const stoneMat = this.materials.createStoneWalkwayMaterial();
    const walkway = (name: string, w: number, d: number, cx: number, cz: number, repeatsX: number, repeatsZ: number) => {
      const slab = MeshBuilder.CreateBox(
        name,
        { width: w, height: 0.04, depth: d, faceUV: ShipBoxMap.tiledUV(repeatsZ, repeatsX) },
        this.scene
      );
      slab.position.set(cx, 0.02, cz);
      slab.material = stoneMat;
      this.collectStatic(stoneMat, slab);
    };
    walkway("walkNS", 2.9, 28.2, 0.15, 0, 2, 19); // north-south lane
    walkway("walkEW_w", 12.8, 2.6, -7.7, 0.25, 9, 2); // east-west lane, west leg
    walkway("walkEW_e", 12.5, 2.6, 7.85, 0.25, 9, 2); // east-west lane, east leg
    walkway("walkRingN", 31.8, 1.8, 0, 15.0, 21, 1); // wall-footing ring
    walkway("walkRingS", 31.8, 1.8, 0, -15.0, 21, 1);
    walkway("walkRingE", 1.8, 28.2, 15.0, 0, 1, 21);
    walkway("walkRingW", 1.8, 28.2, -15.0, 0, 1, 21);

    // Perimeter: concrete walls plastered in graffiti, concrete cap, barbed wire
    const wallMat = this.materials.createGraffitiWallMaterial();
    const capMat = this.materials.createConcreteMaterial();
    const capUV = ShipBoxMap.tiledUV(12, 1);
    const metalMat = this.materials.createMetalMaterial();

    const wallH = 2.55;
    const sides: Array<[string, number, number, number, number]> = [
      // name, cx, cz, width(x), depth(z)
      ["wallN", 0, 16.3, 33.8, 0.6],
      ["wallS", 0, -16.3, 33.8, 0.6],
      ["wallE", 16.3, 0, 0.6, 33.8],
      ["wallW", -16.3, 0, 0.6, 33.8],
    ];
    for (const [row, [name, cx, cz, w, d]] of sides.entries()) {
      this.box(name, w, wallH, d, cx, cz, wallMat, 0, false, null, ShipBoxMap.graffitiWallUV(w, d, row));
      this.box(`${name}_cap`, w + 0.16, 0.15, d + 0.16, cx, cz, capMat, wallH, false, null, capUV);

      // barbed-wire posts + two wire runs along the cap
      const alongX = w > d;
      for (let k = 0; k < 7; k++) {
        const t = -16.1 + k * 5.37;
        this.box(`${name}_post${k}`, 0.07, 0.55, 0.07, alongX ? t : cx, alongX ? cz : t, metalMat, wallH + 0.15, false);
      }
      for (const wy of [0.32, 0.52]) {
        this.box(
          `${name}_wire${wy}`,
          alongX ? 33 : 0.035,
          0.035,
          alongX ? 0.035 : 33,
          cx,
          cz,
          metalMat,
          wallH + 0.15 + wy,
          false
        );
      }
    }
  }

  private addCaster(mesh: Mesh): void {
    if (this.shadowGen) {
      this.shadowGen.addShadowCaster(mesh, true);
    }
  }

  private collectStatic(mat: Material, mesh: Mesh, castShadow: boolean = true): void {
    const map = castShadow ? this.casterParts : this.nonCasterParts;
    const list = map.get(mat);
    if (list) {
      list.push(mesh);
    } else {
      map.set(mat, [mesh]);
    }
  }

  private mergeStaticParts(): void {
    for (const [map, cast] of [
      [this.casterParts, true],
      [this.nonCasterParts, false],
    ] as const) {
      for (const parts of map.values()) {
        const merged = parts.length > 1 ? Mesh.MergeMeshes(parts, true, true, undefined, false, false) : parts[0];
        if (!merged) continue;
        merged.receiveShadows = true;
        merged.freezeWorldMatrix();
        if (cast) this.addCaster(merged);
      }
      map.clear();
    }
  }

  // The freeze must wait for the depth shaders: they compile in parallel,
  // and a RENDER_ONCE pass taken before they're ready skips every caster
  // and bakes an empty (shadowless) map forever.
  private freezeShadows(): void {
    if (!this.shadowGen) return;
    this.shadowMap = this.shadowGen.getShadowMap();
    this.shadowGen.forceCompilationAsync().then(() => {
      this.shadowsReady = true;
      this.applyShadowMode();
    });
    // bodies that land later join the caster list if the tier wants them
    onDynamicCasterAdded.add((mesh) => {
      if (this.dynamicShadows && this.shadowGen) this.shadowGen.addShadowCaster(mesh, false);
    });
  }

  // High tier: the soldiers throw real shadows, so the map re-renders every
  // frame. Lower tiers bake the static yard once and leave the bodies to
  // their blob shadows.
  public setDynamicShadows(on: boolean): void {
    if (this.dynamicShadows === on) return;
    this.dynamicShadows = on;
    if (!this.shadowGen) return;
    for (const mesh of dynamicShadowCasters()) {
      if (on) this.shadowGen.addShadowCaster(mesh, false);
      else this.shadowGen.removeShadowCaster(mesh, false);
    }
    this.applyShadowMode();
  }

  private static rainEmitRate(quality: GraphicsQuality): number {
    return quality === "performance" ? 650 : quality === "balanced" ? 1300 : 2600;
  }

  public setGraphicsQuality(quality: GraphicsQuality): void {
    this.setDynamicShadows(quality === "high");
    if (!this.rain) return;
    const emitRate = ShipBoxMap.rainEmitRate(quality);
    if (this.rain.emitRate === emitRate) return;
    this.rain.emitRate = emitRate;
    // Shed the previous tier's live particles immediately, including when
    // the quality picker is used while paused.
    this.rain.reset();
  }

  public setPaused(paused: boolean): void {
    if (this.rain) this.rain.paused = paused;
  }

  public resetMatch(): void {
    for (const wreck of this.wrecks) wreck.resetGlass();
  }

  private applyShadowMode(): void {
    if (!this.shadowMap || !this.shadowsReady) return;
    this.shadowMap.refreshRate = this.dynamicShadows
      ? RenderTargetTexture.REFRESHRATE_RENDER_ONEVERYFRAME
      : RenderTargetTexture.REFRESHRATE_RENDER_ONCE;
  }

  // Box helper: builds the mesh, registers its AABB, casts/receives shadows
  // Per-face UVs for a box: `wrap` keeps the texture upright on all four
  // sides (Babylon's default rotates it on the ±x faces), and `ref` maps
  // each face proportionally to a reference panel size so a texture painted
  // for a 6.1 m container side shows the same rib spacing on a 2.5 m end
  // The container texture is painted for one 20ft side: 6.1 m by 2.6 m
  private static readonly CONTAINER_PANEL: [number, number] = [6.1, 2.6];

  private static boxUV(w: number, h: number, d: number, ref: [number, number] | null): Vector4[] {
    const face = (fw: number, fh: number): Vector4 =>
      ref ? new Vector4(0, 0, Math.min(1, fw / ref[0]), Math.min(1, fh / ref[1])) : new Vector4(0, 0, 1, 1);
    return [face(w, h), face(w, h), face(d, h), face(d, h), face(w, d), face(w, d)];
  }

  // Each 33.8m face reads one complete 2048×256 atlas strip instead of
  // tiling a square painting. Short faces/top/bottom stay plain concrete.
  // Vertical inset matches WorldMaterials' 8px gutters and keeps bilinear
  // filtering from borrowing a neighbouring wall's paint. DynamicTexture
  // uploads canvas y downward, so the first painted strip is at high v.
  private static graffitiWallUV(w: number, d: number, row: number): Vector4[] {
    const v0 = 1 - (row + 1) / 4 + 8 / 1024;
    const v1 = 1 - row / 4 - 8 / 1024;
    const long = (): Vector4 => new Vector4(0, v0, 1, v1);
    const plain = (): Vector4 => new Vector4(2 / 2048, v0, 24 / 2048, v1);
    return w > d ? [long(), long(), plain(), plain(), plain(), plain()] : [plain(), plain(), long(), long(), plain(), plain()];
  }

  private box(
    name: string,
    w: number,
    h: number,
    d: number,
    x: number,
    z: number,
    mat: Material,
    yBase: number = 0,
    collide: boolean = true,
    uvRef: [number, number] | null = null,
    faceUV: Vector4[] | null = null,
    castShadow: boolean = true
  ): Mesh {
    const m = MeshBuilder.CreateBox(
      name,
      { width: w, height: h, depth: d, wrap: true, faceUV: faceUV ?? ShipBoxMap.boxUV(w, h, d, uvRef) },
      this.scene
    );
    m.position.set(x, yBase + h / 2, z);
    m.material = mat;
    this.collectStatic(mat, m, castShadow);
    if (collide) {
      PlayerController.registerObstacle(x - w / 2, x + w / 2, yBase, yBase + h, z - d / 2, z + d / 2);
    }
    return m;
  }

  private barrel(name: string, x: number, z: number, mat: Material): void {
    const b = MeshBuilder.CreateCylinder(name, { height: 0.95, diameter: 0.62, tessellation: 14 }, this.scene);
    b.position.set(x, 0.475, z);
    b.material = mat;
    this.collectStatic(mat, b);
    PlayerController.registerObstacle(x - 0.31, x + 0.31, 0, 0.95, z - 0.31, z + 0.31);
  }

  private container(
    name: string,
    cx: number,
    cz: number,
    yaw: number,
    mat: Material,
    doorMat: Material | null,
    doorSide: 1 | -1 | 0,
    yBase: number = 0,
    roll: number = 0,
    collide: boolean = true,
    castShadow: boolean = true
  ): void {
    const cosR = Math.cos(roll);
    const sinR = Math.abs(Math.sin(roll));
    const cy = yBase + 1.3 * cosR + 1.25 * sinR; // low long-edge stays grounded

    const body = MeshBuilder.CreateBox(
      name,
      { width: 6.1, height: 2.6, depth: 2.5, wrap: true, faceUV: ShipBoxMap.boxUV(6.1, 2.6, 2.5, ShipBoxMap.CONTAINER_PANEL) },
      this.scene
    );
    body.position.set(cx, cy, cz);
    body.rotation.set(roll, yaw, 0);
    body.computeWorldMatrix(true);
    body.material = mat;
    this.collectStatic(mat, body, castShadow);

    if (doorMat && doorSide !== 0) {
      const dx = Math.cos(yaw) * 3.08 * doorSide;
      const dz = -Math.sin(yaw) * 3.08 * doorSide;
      const door = MeshBuilder.CreateBox(
        `${name}_door`,
        { width: 0.07, height: 2.44, depth: 2.4, wrap: true, faceUV: ShipBoxMap.boxUV(0.07, 2.44, 2.4, null) },
        this.scene
      );
      door.position.set(cx + dx, cy, cz + dz);
      door.rotation.set(roll, yaw, 0);
      door.computeWorldMatrix(true);
      door.material = doorMat;
      this.collectStatic(doorMat, door, castShadow);
    }

    if (collide) {
      const crossHalf = 1.25 * cosR + 1.3 * sinR;
      PlayerController.registerObstacleOBB(
        cx,
        cz,
        3.05 + 0.05,
        crossHalf + 0.05,
        yBase,
        yBase + 2 * (1.3 * cosR + 1.25 * sinR),
        yaw
      );
    }
  }

  // Open container shell: two side walls, roof, end headers, plywood floor.
  // closedEnd 0 = open both ends; ±1 seals that end with a full wall.
  private openContainer(name: string, cx: number, cz: number, axis: "x" | "z", mat: Material, closedEnd: 1 | -1 | 0 = 0): void {
    const len = 6.1;
    const alongX = axis === "x";
    const woodMat = this.materials.createWoodCrateMaterial();
    const P = ShipBoxMap.CONTAINER_PANEL;

    const part = (
      suffix: string,
      wAlong: number,
      h: number,
      wCross: number,
      offAlong: number,
      offCross: number,
      m: Material,
      yBase: number,
      collide: boolean,
      uvRef: [number, number] | null = P
    ): void => {
      this.box(
        `${name}_${suffix}`,
        alongX ? wAlong : wCross,
        h,
        alongX ? wCross : wAlong,
        cx + (alongX ? offAlong : offCross),
        cz + (alongX ? offCross : offAlong),
        m,
        yBase,
        collide,
        uvRef
      );
    };

    part("w1", len, 2.6, 0.1, 0, -1.2, mat, 0, true);
    part("w2", len, 2.6, 0.1, 0, 1.2, mat, 0, true);
    part("roof", len, 0.16, 2.5, 0, 0, mat, 2.44, false);
    part("hdrA", 0.14, 0.3, 2.5, -len / 2 + 0.07, 0, mat, 2.3, false);
    part("hdrB", 0.14, 0.3, 2.5, len / 2 - 0.07, 0, mat, 2.3, false);
    // floor base sits 6mm proud of the stone walkway slabs (no coplanar tops)
    part("floor", len - 0.1, 0.04, 2.4, 0, 0, woodMat, 0.006, false, null);

    const hx = (alongX ? len : 2.5) / 2;
    const hz = (alongX ? 2.5 : len) / 2;
    PlayerController.registerObstacle(cx - hx, cx + hx, 2.44, 2.6, cz - hz, cz + hz);

    if (closedEnd !== 0) {
      part("cap", 0.1, 2.44, 2.3, closedEnd * (len / 2 - 0.05), 0, mat, 0, true);
    }
  }

  // Stack of shipping pallets (rough boards, ~0.65m high)
  private pallet(cx: number, cz: number, yaw: number): void {
    const woodMat = this.materials.createWoodCrateMaterial();
    for (let i = 0; i < 4; i++) {
      const p = MeshBuilder.CreateBox(`pallet_${cx}_${cz}_${i}`, { width: 1.6, height: 0.13, depth: 1.3 }, this.scene);
      p.position.set(cx, 0.065 + i * 0.165, cz);
      p.rotation.y = yaw + (i % 2) * 0.07;
      p.computeWorldMatrix(true);
      p.material = woodMat;
      this.collectStatic(woodMat, p);
    }
    PlayerController.registerObstacleOBB(cx, cz, 0.84, 0.69, 0, 0.63, yaw);
  }

  private crate(cx: number, cz: number, size: number, yaw: number): void {
    const woodMat = this.materials.createWoodCrateMaterial();
    const c = MeshBuilder.CreateBox(`crate_${cx}_${cz}`, { width: size, height: size, depth: size }, this.scene);
    c.position.set(cx, size / 2, cz);
    c.rotation.y = yaw;
    c.computeWorldMatrix(true);
    c.material = woodMat;
    this.collectStatic(woodMat, c);
    const h = size / 2 + 0.03;
    PlayerController.registerObstacleOBB(cx, cz, h, h, 0, size, yaw);
  }

  private carWreck(cx: number, cz: number, yaw: number): void {
    this.wrecks.push(
      new CarWreck(this.scene, this.materials, new Vector3(cx, 0, cz), yaw, (mat, mesh) => this.collectStatic(mat, mesh))
    );
    PlayerController.registerObstacleOBB(cx, cz, 2.2, 0.94, 0, 1.5, yaw);
  }

  private truckWreck(cx: number, cz: number, yaw: number): void {
    this.wrecks.push(
      new TruckWreck(this.scene, this.materials, new Vector3(cx, 0, cz), yaw, (mat, mesh) => this.collectStatic(mat, mesh))
    );
    PlayerController.registerObstacleOBB(cx, cz, 3.25, 1.2, 0, 2.95, yaw);
  }

  // ---------------- layout (coordinates traced from the overhead) ----------------

  private createCenterBlocks(): void {
    const green = this.materials.createContainerMaterial("green");
    const rust = this.materials.createContainerMaterial("rust");
    const blue = this.materials.createContainerMaterial("blue");
    const red = this.materials.createContainerMaterial("red");
    const gray = this.materials.createContainerMaterial("gray");
    const greenDoor = this.materials.createContainerDoorMaterial("green");
    const blueDoor = this.materials.createContainerDoorMaterial("blue");
    const redDoor = this.materials.createContainerDoorMaterial("red");
    const rustDoor = this.materials.createContainerDoorMaterial("rust");
    const grayDoor = this.materials.createContainerDoorMaterial("gray");
    const metalMat = this.materials.createMetalMaterial();

    // NW: green stack
    this.container("nwN_lo", -4.3, 5.16, 0, green, greenDoor, 1);
    this.container("nwN_hi", -4.3, 5.16, 0, green, greenDoor, 1, 2.6);
    this.openContainer("nwOpen", -4.3, 2.64, "x", green, -1); // mouth east, sealed west
    this.container("nwS_hi", -4.3, 2.64, 0, green, greenDoor, 1, 2.6);
    this.barrel("barrelNW1", -7.8, 5.6, metalMat);
    this.barrel("barrelNW2", -7.9, 4.5, metalMat);

    // NE: two CLOSED containers
    this.container("neBack", 4.6, 5.26, 0, blue, blueDoor, -1);
    this.container("neFront", 4.6, 2.74, 0, rust, rustDoor, -1);
    this.barrel("barrelNE1", 0.9, 6.1, metalMat);
    this.barrel("barrelNE2", 0.9, 4.7, metalMat);
    this.barrel("barrelNE3", 7.95, 6.05, metalMat);
    this.barrel("barrelNE4", 7.95, 4.6, metalMat);
    this.crate(9.2, 4.6, 1.3, 0.18);

    // SW: two CLOSED containers
    this.container("swFront", -4.5, -2.14, 0, gray, grayDoor, 1);
    this.container("swClosed", -4.5, -4.66, 0, blue, blueDoor, 1);

    // SE: the red double stack
    for (const rz of [-2.34, -4.86]) {
      this.container(`seLo${rz}`, 4.6, rz, 0, red, redDoor, -1);
      this.container(`seHi${rz}`, 4.6, rz, 0, red, redDoor, -1, 2.6);
    }
    this.barrel("barrelSE1", 7.95, -5.3, metalMat);
  }

  private createEdgeStructures(): void {
    const rust = this.materials.createContainerMaterial("rust");
    const blue = this.materials.createContainerMaterial("blue");
    const gray = this.materials.createContainerMaterial("gray");
    const grayDoor = this.materials.createContainerDoorMaterial("gray");
    const blueDoor = this.materials.createContainerDoorMaterial("blue");

    // North edge
    this.openContainer("nInner", 0, 11.4, "x", rust, 1); // open west, sealed east
    this.container("nOuter", 0, 13.9, 0, gray, grayDoor, -1);

    // South edge
    this.openContainer("sInner", 0, -11.3, "x", blue, -1); // open east, sealed west
    this.container("sOuter", 0, -13.8, 0, gray, grayDoor, 1);
    this.crate(-7.1, -13.85, 1.25, 0.08);
    this.crate(-5.7, -13.85, 1.25, -0.06);
    this.crate(4.6, -13.9, 1.25, 0.12);

    // West edge
    this.openContainer("wOpen", -14.7, 0.6, "z", gray);
    this.container("wClosed", -12.35, 0.75, Math.PI / 2, gray, grayDoor, -1);

    // East edge
    this.openContainer("eOpen", 14.7, 0.35, "z", blue);
    this.container("eClosed", 12.15, 0.6, Math.PI / 2, blue, blueDoor, -1);
  }

  private createCornerClutter(): void {
    const metalMat = this.materials.createMetalMaterial();

    // NW: pallet + crate
    this.pallet(-10.1, 9.6, 0.5);
    this.crate(-6.6, 10.0, 1.3, -0.3);

    // NE: white box truck
    this.truckWreck(9.9, 10.2, -0.6 + Math.PI / 2);

    // SW: abandoned car + barrels
    this.carWreck(-10.9, -9.9, 0.65);
    this.barrel("barrelSW1", -8.4, -11.2, metalMat);
    this.barrel("barrelSW2", -7.9, -11.8, metalMat);

    // SE: pallets
    this.pallet(11.4, -10.5, 0.55);
    this.pallet(11.8, -6.6, -0.3);
  }

  // Everything past the walls is set dressing: no collision, no shadow casting
  private createOutOfBounds(): void {
    const concreteMat = this.materials.createConcreteMaterial();
    const oobConcreteUV = ShipBoxMap.tiledUV(8, 2);
    const tallConcreteUV = ShipBoxMap.tiledUV(6, 3);
    const metalMat = this.materials.createMetalMaterial();
    const windowMat = this.materials.createWindowBandMaterial();
    const green = this.materials.createContainerMaterial("green");
    const rust = this.materials.createContainerMaterial("rust");
    const blue = this.materials.createContainerMaterial("blue");
    const gray = this.materials.createContainerMaterial("gray");
    const darkMat = flatMat(this.scene, "wreckDarkMat", { albedo: [0.1, 0.1, 0.11], rough: 0.8 });

    const oobBox = (
      name: string,
      w: number,
      h: number,
      d: number,
      x: number,
      z: number,
      mat: Material,
      yBase: number = 0,
      faceUV: Vector4[] | null = null
    ) => this.box(name, w, h, d, x, z, mat, yBase, false, null, faceUV, false);

    const oobContainer = (name: string, cx: number, cz: number, yaw: number, mat: Material, yBase: number = 0) =>
      this.container(name, cx, cz, yaw, mat, null, 0, yBase, 0, false, false);

    // -- Warehouses, yard clutter and the crane above the north roof --
    const oobBoxes: ReadonlyArray<[string, number, number, number, number, number, Material, number?, Vector4[]?]> = [
      ["warehouseN", 30, 10, 12, -2, 27, concreteMat, 0, tallConcreteUV],
      ["warehouseN_win1", 22, 1.6, 0.12, -2, 20.96, windowMat, 5.2],
      ["warehouseN_win2", 14, 1.2, 0.12, -2, 20.96, windowMat, 7.8],
      ["warehouseN_door1", 3.4, 3.8, 0.12, -6, 20.97, darkMat],
      ["warehouseN_door2", 3.4, 3.8, 0.12, 3, 20.97, darkMat],
      ["warehouseN_parapet", 30.4, 0.6, 0.3, -2, 21.14, concreteMat, 10, oobConcreteUV],
      ["warehouseN_vent1", 2.2, 1.2, 1.8, -8, 25, metalMat, 10],
      ["warehouseN_vent2", 2.2, 1.2, 1.8, 4, 25, metalMat, 10],
      ["warehouseN_ventcap1", 2.6, 0.18, 2.2, -8, 25, concreteMat, 11.2, oobConcreteUV],
      ["warehouseN_ventcap2", 2.6, 0.18, 2.2, 4, 25, concreteMat, 11.2, oobConcreteUV],
      ["warehouseW", 12, 9, 28, -28, 1, concreteMat, 0, tallConcreteUV],
      ["warehouseW_win1", 0.12, 1.5, 22, -22.0, 1, windowMat, 4.5],
      ["warehouseW_win2", 0.12, 1.0, 14, -22.0, 1, windowMat, 7.2],
      ["warehouseW_door", 0.12, 3.5, 3.0, -22.0, -6, darkMat],
      ["warehouseW_parapet", 0.3, 0.55, 28.4, -22.14, 1, concreteMat, 9, oobConcreteUV],
      ["warehouseE", 12, 9, 26, 28, -2, concreteMat, 0, tallConcreteUV],
      ["warehouseE_win1", 0.12, 1.5, 20, 22.0, -2, windowMat, 4.5],
      ["warehouseE_win2", 0.12, 1.0, 12, 22.0, -2, windowMat, 7.2],
      ["warehouseE_door", 0.12, 3.5, 3.0, 22.0, 4, darkMat],
      ["warehouseE_parapet", 0.3, 0.55, 26.4, 22.14, -2, concreteMat, 9, oobConcreteUV],
      ["warehouseE_sign", 0.1, 1.0, 4.5, 22.06, -8, darkMat, 6],
      ["dumpsterS1", 2.0, 1.3, 1.0, 14, -19, darkMat],
      ["dumpsterS1_lid", 2.1, 0.08, 1.1, 14, -19, metalMat, 1.3],
      ["dumpsterE1", 1.0, 1.3, 2.0, 20, 12, darkMat],
      ["dumpsterE1_lid", 1.1, 0.08, 2.1, 20, 12, metalMat, 1.3],
      ["gantry_w", 0.6, 18.5, 0.8, -5, 35, metalMat],
      ["gantry_e", 0.6, 18.5, 0.8, 9, 35, metalMat],
      ["gantry_beam", 22, 0.72, 0.85, 2, 35, rust, 18.5],
      ["gantry_rail", 22, 0.14, 0.9, 2, 35, metalMat, 19.22],
      ["gantry_trolley", 2.4, 0.55, 1.25, 1, 35, metalMat, 18],
      ["gantry_cable1", 0.1, 2.7, 0.1, 0, 35, metalMat, 15.3],
      ["gantry_cable2", 0.1, 2.7, 0.1, 2, 35, metalMat, 15.3],
      ["gantry_load", 4.8, 2, 2, 1, 35, rust, 13.3],
      ["junctionBox", 0.55, 0.55, 0.1, -8, -16.8, metalMat, 1.4],
    ];
    for (const [name, w, h, d, x, z, mat, yBase, faceUV] of oobBoxes) {
      oobBox(name, w, h, d, x, z, mat, yBase, faceUV);
    }
    for (const [name, height, diameter, x, y, z] of [
      ["exhaust1", 2.2, 0.35, -12, 11.1, 24.5],
      ["exhaust2", 2.8, 0.4, 24.5, 10.4, -10],
    ] as const) {
      const exhaust = MeshBuilder.CreateCylinder(name, { height, diameter, tessellation: 10 }, this.scene);
      exhaust.position.set(x, y, z);
      exhaust.material = metalMat;
      this.collectStatic(metalMat, exhaust, false);
    }

    // -- Container stacks --
    const oobContainers: ReadonlyArray<[string, number, number, number, Material, number]> = [
      ["oobStackS1a", 6, -23, 0.15, green, 0],
      ["oobStackS1b", 6, -23, 0.15, green, 2.6],
      ["oobStackS1c", 6.3, -23, 0.15, rust, 5.2],
      ["oobStackS2a", -9, -24, -0.2, blue, 0],
      ["oobStackS2b", -8.8, -24.1, -0.18, gray, 2.6],
      ["oobStackS3", 16, -22, 0.05, rust, 0],
      ["oobStackE1a", 21, 8, 0.1, rust, 0],
      ["oobStackE1b", 21, 8, 0.1, rust, 2.6],
      ["oobStackE1c", 21.2, 8.1, 0.12, blue, 5.2],
      ["oobStackE2", 24, 1, -0.05, green, 0],
      ["oobStackW1a", -24, -8, 0.08, gray, 0],
      ["oobStackW1b", -24, -8, 0.08, blue, 2.6],
    ];
    for (const [name, cx, cz, yaw, mat, yBase] of oobContainers) oobContainer(name, cx, cz, yaw, mat, yBase);

    // -- Water tower --
    const tx = 10,
      tz = 30;
    for (const s of [-1.6, 1.6]) {
      for (const z of [-1.6, 1.6]) oobBox(`tower_leg_${s}_${z}`, 0.22, 13, 0.22, tx + s, tz + z, metalMat);
      for (const [tag, y] of [
        ["lo", 3.2],
        ["hi", 7.4],
        ["mid", 5.4],
      ] as const) {
        oobBox(`tower_brace_${tag}_${s}`, 3.6, 0.14, 0.14, tx, tz + s, metalMat, y);
      }
      for (const [tag, y] of [
        ["lo", 3.2],
        ["hi", 7.4],
      ] as const) {
        oobBox(`tower_brace2_${tag}_${s}`, 0.14, 0.14, 3.6, tx + s, tz, metalMat, y);
      }
    }
    const tank = MeshBuilder.CreateCylinder("tower_tank", { height: 3.8, diameter: 5.0, tessellation: 18 }, this.scene);
    tank.position.set(tx, 14.9, tz);
    tank.material = metalMat;
    this.collectStatic(metalMat, tank, false);
    const cone = MeshBuilder.CreateCylinder(
      "tower_cone",
      { height: 1.8, diameterTop: 0.5, diameterBottom: 5.2, tessellation: 18 },
      this.scene
    );
    cone.position.set(tx, 17.7, tz);
    cone.material = metalMat;
    this.collectStatic(metalMat, cone, false);
    for (const ry of [13.6, 15.8]) {
      const hoop = MeshBuilder.CreateCylinder(
        `tower_hoop_${ry}`,
        { diameter: 5.12, height: 0.12, tessellation: 18, cap: Mesh.NO_CAP },
        this.scene
      );
      hoop.position.set(tx, ry, tz);
      hoop.material = metalMat;
      this.collectStatic(metalMat, hoop, false);
    }
    oobBox("tower_pipe", 0.1, 4.0, 0.1, tx + 2.6, tz, metalMat, 10);

    // -- Yard light poles --
    const lampMat = flatMat(this.scene, "lampHeadMat", { albedo: [0.2, 0.2, 0.2], rough: 0.6, emissive: [0.55, 0.58, 0.52] });
    const poles: Array<[number, number, number, number]> = [
      [-20, 11, 1, 0],
      [22, -1, -1, 0],
      [7, 23, 0, -1],
      [-18, -14, 0.7, 0.7],
    ];
    for (const [px, pz, dx, dz] of poles) {
      const pole = MeshBuilder.CreateCylinder(`lamp_${px}_${pz}`, { height: 8.5, diameter: 0.22, tessellation: 10 }, this.scene);
      pole.position.set(px, 4.25, pz);
      pole.material = metalMat;
      this.collectStatic(metalMat, pole, false);
      const collar = MeshBuilder.CreateCylinder(
        `lamp_collar_${px}_${pz}`,
        { height: 0.4, diameterTop: 0.32, diameterBottom: 0.56, tessellation: 10 },
        this.scene
      );
      collar.position.set(px, 0.2, pz);
      collar.material = metalMat;
      this.collectStatic(metalMat, collar, false);
      const arm = MeshBuilder.CreateBox(`lamp_arm_${px}_${pz}`, { width: 1.6, height: 0.12, depth: 0.12 }, this.scene);
      arm.position.set(px + dx * 0.8, 8.35, pz + dz * 0.8);
      arm.rotation.y = Math.atan2(-dz, dx);
      arm.computeWorldMatrix(true);
      arm.material = metalMat;
      this.collectStatic(metalMat, arm, false);
      const head = MeshBuilder.CreateBox(`lamp_head_${px}_${pz}`, { width: 0.62, height: 0.18, depth: 0.32 }, this.scene);
      head.position.set(px + dx * 1.55, 8.22, pz + dz * 1.55);
      head.rotation.y = Math.atan2(-dz, dx);
      head.computeWorldMatrix(true);
      head.material = lampMat;
      this.collectStatic(lampMat, head, false);
    }

    // -- Utility pipes --
    for (let pi = 0; pi < 3; pi++) {
      const pipe = MeshBuilder.CreateCylinder(`wallpipe_${pi}`, { height: 34, diameter: 0.18, tessellation: 8 }, this.scene);
      pipe.rotation.z = Math.PI / 2;
      pipe.computeWorldMatrix(true);
      pipe.position.set(0, 0.55 + pi * 0.38, -16.85);
      pipe.material = metalMat;
      this.collectStatic(metalMat, pipe, false);
    }

    // Billboards
    const billH = 5.0;
    const billFrame = 0.7;
    const b1Mat = this.materials.createBillboardMuralMaterial(1);
    const b2Mat = this.materials.createBillboardMuralMaterial(2);
    const b2Size = b2Mat.albedoTexture!.getSize();
    const b2W = billH * (b2Size.width / b2Size.height);
    for (const [prefix, mat, yBase, bw, bd, bx, bz, pw, px, pz, ry] of [
      ["billN", b1Mat, 10.0 - (billH + billFrame), 4.3, 0.14, -5, 20.93, billH * 0.75, -5, 20.82, 0],
      ["billE", b2Mat, 9.0 - (billH + billFrame), 0.14, b2W + 0.55, 21.93, 2, b2W, 21.82, 2, Math.PI / 2],
    ] as const) {
      oobBox(`${prefix}_back`, bw, billH + billFrame, bd, bx, bz, darkMat, yBase);
      const plane = MeshBuilder.CreatePlane(
        `${prefix}_img`,
        { width: pw, height: billH, sideOrientation: Mesh.DOUBLESIDE },
        this.scene
      );
      plane.position.set(px, yBase + (billH + billFrame) / 2, pz);
      if (ry !== 0) {
        plane.rotation.y = ry;
        plane.computeWorldMatrix(true);
      }
      plane.material = mat;
      plane.receiveShadows = true;
      plane.freezeWorldMatrix();
    }
  }

  private createLongGrass(): void {
    const mat = new PBRMaterial("grassTuftMat", this.scene);
    mat.albedoTexture = this.materials.createGrassBladeTexture();
    mat.useAlphaFromAlbedoTexture = true;
    mat.transparencyMode = 1; // alpha-test cutout
    mat.alphaCutOff = 0.02;
    mat.backFaceCulling = false;
    mat.twoSidedLighting = true;
    mat.metallic = 0;
    mat.roughness = 0.85;
    mat.specularIntensity = 0.35;
    mat.emissiveColor = new Color3(0.04, 0.05, 0.035);

    const quadA = MeshBuilder.CreatePlane("tuftA", { width: 0.72, height: 0.62 }, this.scene);
    quadA.position.y = 0.31;
    const quadB = quadA.clone("tuftB");
    quadB.rotation.y = Math.PI / 3;
    const quadC = quadA.clone("tuftC");
    quadC.rotation.y = -Math.PI / 3;
    const tuft = Mesh.MergeMeshes([quadA, quadB, quadC], true, true, undefined, false, false);
    if (!tuft) return;
    tuft.material = mat;
    tuft.isPickable = false;
    tuft.alwaysSelectAsActiveMesh = true;
    tuft.receiveShadows = false;

    const clear: Array<[number, number, number, number]> = [
      [-1.3, 1.6, -14.1, 14.1],
      [-14.1, 14.1, -1.05, 1.55],
      [-15.9, 15.9, 14.1, 15.9],
      [-15.9, 15.9, -15.9, -14.1],
      [14.1, 15.9, -14.1, 14.1],
      [-15.9, -14.1, -14.1, 14.1],
      [-15.95, -11.1, -2.9, 3.85],
      [11.15, 15.95, -2.8, 3.85],
      [-3.1, 3.1, 10.1, 12.7],
      [-3.1, 3.1, -12.6, -10.0],
      [-7.4, -1.2, 1.35, 3.95],
      [-15, 9, 21, 31],
      [-32, -22, -10, 14],
      [22, 32, -14, 8],
      [2, 10, -25.5, -20.5],
      [-12.5, -5.5, -26, -22],
      [17.5, 24.5, 5.5, 10.5],
    ];
    const obstacles = PlayerController.getObstacles();

    const open = (x: number, z: number): boolean => {
      for (const [a, b, c, d] of clear) {
        if (x > a && x < b && z > c && z < d) return false;
      }
      for (const obs of obstacles) {
        if (obs.minY > 0.5) continue;
        const dx = x - obs.cx;
        const dz = z - obs.cz;
        const cos = Math.cos(obs.yaw);
        const sin = Math.sin(obs.yaw);
        if (Math.abs(cos * dx - sin * dz) < obs.hw + 0.12 && Math.abs(sin * dx + cos * dz) < obs.hd + 0.12) {
          return false;
        }
      }
      return true;
    };

    const matrices: number[] = [];
    const colors: number[] = [];
    const tmpMat = new Matrix();
    const tmpScale = new Vector3();
    const tmpPos = new Vector3();
    const drop = (px: number, pz: number): void => {
      const sy = 0.78 + Math.random() * 0.85;
      const sxz = 0.8 + Math.random() * 0.55;
      const patch = 0.5 + 0.5 * Math.sin(px * 0.37 + 1.3) * Math.cos(pz * 0.29);
      const shade = 0.55 + Math.random() * 0.35 + patch * 0.25;
      const yellow = Math.random() < 0.18 ? 0.18 : 0;
      colors.push(shade * (0.9 + yellow), shade * (1.0 + yellow * 0.4), shade * (0.8 - yellow * 0.4), 1);
      tmpScale.set(sxz, sy, sxz);
      tmpPos.set(px, 0, pz);
      Matrix.ComposeToRef(
        tmpScale,
        Quaternion.RotationYawPitchRoll(Math.random() * Math.PI, (Math.random() - 0.5) * 0.12, 0),
        tmpPos,
        tmpMat
      );
      for (let i = 0; i < 16; i++) matrices.push(tmpMat.m[i]);
    };

    for (let gx = -15.7; gx <= 15.7; gx += 0.62) {
      for (let gz = -15.7; gz <= 15.7; gz += 0.62) {
        if (Math.random() < 0.18) continue;
        const px = gx + (Math.random() - 0.5) * 0.5;
        const pz = gz + (Math.random() - 0.5) * 0.5;
        if (open(px, pz)) drop(px, pz);
      }
    }
    for (let gx = -40; gx <= 40; gx += 1.3) {
      for (let gz = -40; gz <= 40; gz += 1.3) {
        if (Math.abs(gx) < 16.4 && Math.abs(gz) < 16.4) continue;
        if (Math.random() < 0.3) continue;
        const px = gx + (Math.random() - 0.5) * 1.0;
        const pz = gz + (Math.random() - 0.5) * 1.0;
        if (open(px, pz)) drop(px, pz);
      }
    }

    tuft.thinInstanceSetBuffer("matrix", new Float32Array(matrices), 16, true);
    tuft.thinInstanceSetBuffer("color", new Float32Array(colors), 4, true);
  }

  private createRain(): void {
    const rain = new ParticleSystem("rain", 2400, this.scene);
    this.rain = rain;
    rain.particleTexture = this.materials.createRainStreakTexture();
    rain.emitter = this.rainAnchor;
    rain.minEmitBox = new Vector3(-11, 0, -11);
    rain.maxEmitBox = new Vector3(11, 2.5, 11);
    rain.direction1 = new Vector3(-0.16, -1, -0.06);
    rain.direction2 = new Vector3(-0.1, -1, -0.03);
    rain.minEmitPower = 11.5;
    rain.maxEmitPower = 14;
    rain.updateSpeed = 1 / 60;
    rain.minLifeTime = 0.75;
    rain.maxLifeTime = 0.85;
    rain.emitRate = ShipBoxMap.rainEmitRate(Settings.getGraphicsQuality());
    rain.billboardMode = ParticleSystem.BILLBOARDMODE_STRETCHED;
    rain.minScaleX = 0.025;
    rain.maxScaleX = 0.04;
    rain.minScaleY = 0.9;
    rain.maxScaleY = 1.3;
    rain.color1 = new Color4(0.85, 0.9, 0.97, 0.6);
    rain.color2 = new Color4(0.7, 0.76, 0.85, 0.48);
    rain.colorDead = new Color4(0.7, 0.76, 0.85, 0);
    rain.blendMode = ParticleSystem.BLENDMODE_STANDARD;
    rain.preWarmCycles = 24;
    rain.preWarmStepOffset = 2;
    rain.start();
  }

  public update(_deltaTime?: number): void {
    // Keep the rain volume over the player's head
    const cam = this.scene.activeCamera;
    if (cam) {
      this.rainAnchor.x = cam.globalPosition.x;
      this.rainAnchor.z = cam.globalPosition.z;
    }
  }
}
