import { Mesh } from "@babylonjs/core/Meshes/mesh";
import { MeshBuilder } from "@babylonjs/core/Meshes/meshBuilder";
import { DynamicTexture } from "@babylonjs/core/Materials/Textures/dynamicTexture";
import { TransformNode } from "@babylonjs/core/Meshes/transformNode";
import type { FreeCamera, Scene } from "@babylonjs/core";
import { PlayerController } from "../player/PlayerController";
import type { Input } from "../engine/Input";
import type { Bot } from "../bots/Bot";
import { stdMat } from "../rendering/materials/canvas";
import { CameraDevice, placePiece, smoothstep } from "./CameraDevice";
import { drawHeadingWedge, paintObstacles } from "../ui/tacticalMap";

// The Fall of Duty airstrike ritual: press 4 and the rifle drops out of frame while
// a field laptop swings up, its screen a live north-up tactical map of the
// yard. The mouse stops steering the view and starts steering the targeting
// cursor on that screen; left-click marks the strike, 4 slams the lid shut.
//
// The laptop is a camera-parented viewmodel like the weapons; the display is
// one DynamicTexture repainted only while the lid is up (a baked static
// layer carries the map geometry, so the per-frame cost is blips + cursor).

export type LaptopPhase = "away" | "raising" | "out" | "lowering";

const RAISE_TIME = 0.3;
const LOWER_TIME = 0.22;
const PAINT_INTERVAL = 0.05; // ~20 Hz screen refresh

// screen texture layout
const TEX_W = 512;
const TEX_H = 352;
const MAP_X = 118; // the square map area, centered between telemetry gutters
const MAP_Y = 42;
const MAP_SIDE = 272;

export class AirstrikeLaptop extends CameraDevice {
  private lid: TransformNode; // hinge at the back edge of the base
  private keysTex: DynamicTexture;
  private screenTex: DynamicTexture;
  private staticLayer: HTMLCanvasElement | null = null;

  private phase: LaptopPhase = "away";
  private paintTimer = 0;

  // cursor in screen px, kept inside the map square
  private cursorX = MAP_X + MAP_SIDE / 2;
  private cursorY = MAP_Y + MAP_SIDE / 2;
  private pulse = 0;

  constructor(scene: Scene, camera: FreeCamera) {
    super("airstrikeLaptop", scene);

    const body = stdMat(scene, "laptopBodyMat", { diffuse: [0.16, 0.17, 0.16], spec: [0.08, 0.08, 0.08] });

    // Base: chassis + painted keyboard deck
    placePiece(MeshBuilder.CreateBox("laptopBase", { width: 0.36, height: 0.022, depth: 0.24 }, scene), body, this.root, 0, 0, 0);
    this.keysTex = new DynamicTexture("laptopKeysTex", { width: 256, height: 160 }, scene, false);
    const kctx = this.keysTex.getContext() as CanvasRenderingContext2D;
    kctx.fillStyle = "#1c1e1c";
    kctx.fillRect(0, 0, 256, 160);
    kctx.fillStyle = "#2a2d2a";
    for (let r = 0; r < 5; r++) {
      for (let c = 0; c < 14; c++) {
        kctx.fillRect(6 + c * 18, 10 + r * 22, 14, 16);
      }
    }
    kctx.fillRect(70, 118, 120, 30); // spacebar + trackpad strip
    this.keysTex.update();
    const keysMat = stdMat(scene, "laptopKeysMat", { tex: this.keysTex, spec: [0, 0, 0] });
    placePiece(
      MeshBuilder.CreatePlane("laptopKeys", { width: 0.34, height: 0.22 }, scene),
      keysMat,
      this.root,
      0,
      0.0115,
      0,
      Math.PI / 2
    );

    // Lid: hinged at the back edge, screen plane glowing on its inner face
    this.lid = new TransformNode("laptopLid", scene);
    this.lid.parent = this.root;
    this.lid.position.set(0, 0.011, 0.118);
    placePiece(
      MeshBuilder.CreateBox("laptopLidShell", { width: 0.36, height: 0.255, depth: 0.014 }, scene),
      body,
      this.lid,
      0,
      0.117,
      0.004
    );
    this.screenTex = new DynamicTexture("laptopScreenTex", { width: TEX_W, height: TEX_H }, scene, false);
    const screenMat = stdMat(scene, "laptopScreenMat", { diffuse: [0, 0, 0], spec: [0, 0, 0] });
    screenMat.emissiveTexture = this.screenTex;
    screenMat.disableLighting = true;
    const screen = MeshBuilder.CreatePlane(
      "laptopScreen",
      { width: 0.325, height: 0.215, sideOrientation: Mesh.DOUBLESIDE },
      scene
    );
    placePiece(screen, screenMat, this.lid, 0, 0.117, -0.0045);
    this.lid.rotation.x = 1.45; // folded shut until raised

    this.finalizeRig(camera, 0.32, 0, 0, [this.lid]); // tilted up toward the operator's face
    this.paintScreen(null, [], false); // first frame ready before the first open
  }

  public get isOut(): boolean {
    return this.phase === "out" || this.phase === "raising";
  }

  public get isVisible(): boolean {
    return this.phase !== "away";
  }

  public open(): void {
    if (this.phase === "away" || this.phase === "lowering") {
      this.phase = "raising";
      this.phaseT = 0;
      this.paintTimer = PAINT_INTERVAL;
      this.root.setEnabled(true);
      this.cursorX = MAP_X + MAP_SIDE / 2;
      this.cursorY = MAP_Y + MAP_SIDE / 2;
    }
  }

  public close(): void {
    if (this.phase === "out" || this.phase === "raising") {
      this.phase = "lowering";
      this.phaseT = 0;
    }
  }

  public forceClose(): void {
    this.phase = "away";
    this.paintTimer = 0;
    this.root.setEnabled(false);
  }

  public dispose(): void {
    this.staticLayer = null;
    this.keysTex.dispose();
    this.screenTex.dispose();
    this.root.dispose();
  }

  // Runs every frame. Returns the marked world point on the frame the
  // operator clicks, null otherwise.
  public update(
    dt: number,
    input: Input,
    player: PlayerController,
    bots: ReadonlyArray<Bot>,
    uavActive: boolean
  ): { x: number; z: number } | null {
    if (this.phase === "away") return null;
    this.pulse += dt;

    // Slide up from below while the lid unfolds; reverse on the way down
    if (this.phase === "raising" || this.phase === "lowering") {
      this.phaseT += dt;
      const dur = this.phase === "raising" ? RAISE_TIME : LOWER_TIME;
      let k = Math.min(1, this.phaseT / dur);
      if (this.phase === "lowering") k = 1 - k;
      const s = smoothstep(k);
      this.root.position.set(0.015, -0.55 + s * 0.335, 0.4);
      this.lid.rotation.x = 1.45 - s * 1.78; // shut -> leaned back ~104 degrees
      if (this.phaseT >= dur) {
        if (this.phase === "raising") this.phase = "out";
        else {
          this.phase = "away";
          this.root.setEnabled(false);
          return null;
        }
      }
    }

    let marked: { x: number; z: number } | null = null;
    let cursorMoved = false;
    if (this.phase === "out") {
      // the mouse is the cursor now (PlayerController's look is locked)
      const delta = input.getMouseDelta();
      const nextX = Math.min(MAP_X + MAP_SIDE, Math.max(MAP_X, this.cursorX + delta.x * 0.62));
      const nextY = Math.min(MAP_Y + MAP_SIDE, Math.max(MAP_Y, this.cursorY + delta.y * 0.62));
      cursorMoved = nextX !== this.cursorX || nextY !== this.cursorY;
      this.cursorX = nextX;
      this.cursorY = nextY;
      if (input.isMouseButtonPressed(0)) {
        marked = { x: this.mapToWorldX(this.cursorX), z: this.mapToWorldZ(this.cursorY) };
      }
    }

    if (this.phase !== "lowering") {
      this.paintTimer += dt;
      if (cursorMoved || this.paintTimer >= PAINT_INTERVAL) {
        this.paintTimer %= PAINT_INTERVAL;
        this.paintScreen(player, bots, uavActive);
      }
    }
    return marked;
  }

  // ------------------------------------------------------------- the screen

  private worldToMapX(wx: number): number {
    return MAP_X + ((wx - PlayerController.MAP_MIN_X) / (PlayerController.MAP_MAX_X - PlayerController.MAP_MIN_X)) * MAP_SIDE;
  }
  private worldToMapY(wz: number): number {
    return MAP_Y + ((PlayerController.MAP_MAX_Z - wz) / (PlayerController.MAP_MAX_Z - PlayerController.MAP_MIN_Z)) * MAP_SIDE;
  }
  private mapToWorldX(mx: number): number {
    return PlayerController.MAP_MIN_X + ((mx - MAP_X) / MAP_SIDE) * (PlayerController.MAP_MAX_X - PlayerController.MAP_MIN_X);
  }
  private mapToWorldZ(my: number): number {
    return PlayerController.MAP_MAX_Z - ((my - MAP_Y) / MAP_SIDE) * (PlayerController.MAP_MAX_Z - PlayerController.MAP_MIN_Z);
  }

  // Map geometry, frame, header and telemetry gutters never change — bake once
  private getStaticLayer(): HTMLCanvasElement {
    if (this.staticLayer) return this.staticLayer;
    const layer = document.createElement("canvas");
    layer.width = TEX_W;
    layer.height = TEX_H;
    const c = layer.getContext("2d") as CanvasRenderingContext2D;

    c.fillStyle = "#06131a";
    c.fillRect(0, 0, TEX_W, TEX_H);
    // header / footer bars
    c.fillStyle = "#0c2430";
    c.fillRect(0, 0, TEX_W, 28);
    c.fillRect(0, TEX_H - 26, TEX_W, 26);
    c.fillStyle = "#7ddfa8";
    c.font = "bold 13px monospace";
    c.textBaseline = "middle";
    c.fillText("TACSAT LINK // F-15 STRIKE PACKAGE", 12, 14);
    c.fillStyle = "#9fb7c4";
    c.font = "11px monospace";
    c.fillText("MOVE: TRACK CURSOR   LMB: MARK TARGET   4: ABORT", 12, TEX_H - 13);

    // telemetry gutters — set dressing that sells the uplink
    c.fillStyle = "#1d3a48";
    c.font = "10px monospace";
    const gutterLines = ["SAT 7 LOCK", "GRID 31.2M", "WND 04 KT", "AUTH K9-22", "PKG  3 × 3", "ALT 8200FT"];
    gutterLines.forEach((line, i) => {
      c.fillText(line, 14, 56 + i * 22);
      c.fillText(line, MAP_X + MAP_SIDE + 14, 56 + i * 22);
    });

    // the map square: grid, boundary, obstacles (north-up)
    c.fillStyle = "#08222e";
    c.fillRect(MAP_X, MAP_Y, MAP_SIDE, MAP_SIDE);
    c.strokeStyle = "rgba(80, 170, 150, 0.14)";
    c.lineWidth = 1;
    for (let g = 0; g <= 8; g++) {
      const p = MAP_X + (MAP_SIDE / 8) * g;
      c.beginPath();
      c.moveTo(p, MAP_Y);
      c.lineTo(p, MAP_Y + MAP_SIDE);
      c.stroke();
      const q = MAP_Y + (MAP_SIDE / 8) * g;
      c.beginPath();
      c.moveTo(MAP_X, q);
      c.lineTo(MAP_X + MAP_SIDE, q);
      c.stroke();
    }
    c.strokeStyle = "rgba(125, 223, 168, 0.5)";
    c.lineWidth = 2;
    c.strokeRect(MAP_X, MAP_Y, MAP_SIDE, MAP_SIDE);

    const scale = MAP_SIDE / (PlayerController.MAP_MAX_X - PlayerController.MAP_MIN_X);
    c.fillStyle = "rgba(150, 190, 200, 0.4)";
    paintObstacles(
      c,
      scale,
      (wx) => this.worldToMapX(wx),
      (wz) => this.worldToMapY(wz)
    );

    this.staticLayer = layer;
    return layer;
  }

  private paintScreen(player: PlayerController | null, bots: ReadonlyArray<Bot>, uavActive: boolean): void {
    const c = this.screenTex.getContext() as CanvasRenderingContext2D;
    c.drawImage(this.getStaticLayer(), 0, 0);

    if (player) {
      // operator: white wedge pointing the way the player faces (north-up map)
      const px = this.worldToMapX(player.position.x);
      const py = this.worldToMapY(player.position.z);
      c.save();
      c.translate(px, py);
      c.rotate(player.yaw);
      c.fillStyle = "#f0f3f6";
      drawHeadingWedge(c, 5, -7, 2.5, 6);
      c.fill();
      c.restore();

      // hostiles: radar rules apply — recent fire paints them, UAV paints all
      c.fillStyle = "#ff4d40";
      for (const bot of bots) {
        if (bot.dead) continue;
        if (!uavActive && bot.radarTimer <= 0) continue;
        c.beginPath();
        c.arc(this.worldToMapX(bot.position.x), this.worldToMapY(bot.position.z), 4, 0, Math.PI * 2);
        c.fill();
      }
    }

    // the targeting cursor: crosshair + the blast footprint ring, breathing
    if (this.phase === "out") {
      const ringR = 5.6 * (MAP_SIDE / (PlayerController.MAP_MAX_X - PlayerController.MAP_MIN_X));
      const breathe = 1 + Math.sin(this.pulse * 6) * 0.06;
      c.strokeStyle = "#ffd24d";
      c.lineWidth = 1.5;
      c.beginPath();
      c.arc(this.cursorX, this.cursorY, ringR * breathe, 0, Math.PI * 2);
      c.stroke();
      c.lineWidth = 2;
      c.beginPath();
      c.moveTo(this.cursorX - 10, this.cursorY);
      c.lineTo(this.cursorX + 10, this.cursorY);
      c.moveTo(this.cursorX, this.cursorY - 10);
      c.lineTo(this.cursorX, this.cursorY + 10);
      c.stroke();
    }

    this.screenTex.update();
  }
}
