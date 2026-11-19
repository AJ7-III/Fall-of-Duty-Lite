import { Color3 } from "@babylonjs/core/Maths/math.color";
import { DynamicTexture } from "@babylonjs/core/Materials/Textures/dynamicTexture";
import { RawTexture } from "@babylonjs/core/Materials/Textures/rawTexture";
import { StandardMaterial } from "@babylonjs/core/Materials/standardMaterial";
import { Texture } from "@babylonjs/core/Materials/Textures/texture";
import type { PBRMaterial, Scene } from "@babylonjs/core";
import {
  canvasMat,
  flatMat,
  makeCanvasTexture,
  normalMapFromHeight,
  paintNoise,
  paintScratches,
  paintStreaks,
} from "../../rendering/materials/canvas";
import type { SharedWetMaps } from "../../rendering/materials/canvas";
import { paintFreightPoster, paintGraffitiAtlas, paintRadioPoster, weatherPoster } from "./yardArtwork";

const CONTAINER_PALETTE = {
  green: { base: "#4a5d44", sideShade: "#3c4d38", doorShade: "#41523c" },
  rust: { base: "#7d4a32", sideShade: "#6a3e2a", doorShade: "#6e412c" },
  blue: { base: "#3a566e", sideShade: "#2f4759", doorShade: "#334b60" },
  red: { base: "#71382e", sideShade: "#5e2e26", doorShade: "#633129" },
  gray: { base: "#878a82", sideShade: "#74776f", doorShade: "#7a7d74" },
};
type ContainerColor = keyof typeof CONTAINER_PALETTE;

// Every surface of the Ship Box yard, painted procedurally at load time.
// Materials are cached by name on the scene, so the map and the wrecks can
// ask for the same finish without duplicating textures.
export class WorldMaterials {
  private scene: Scene;
  private containerHeight: Float32Array | null = null;
  private containerBump: DynamicTexture | null = null;
  private containerDoorHeight: Float32Array | null = null;
  private containerDoorBump: DynamicTexture | null = null;
  // One puddle field + ORM map per container family: the colours only differ in paint
  private containerWet: SharedWetMaps = {};
  private containerDoorWet: SharedWetMaps = {};

  constructor(scene: Scene) {
    this.scene = scene;
  }

  // Weathered poured concrete with pocks, chips, grime streaks and cracks
  // Cached once per scene at (1, 1); meshes scale UVs via geometry faceUV.
  public createConcreteMaterial(): PBRMaterial {
    return canvasMat(this.scene, "concreteMat", 512, { rough: 0.86, wet: 0.45, roughVar: 0.3, bump: 1.4 }, (ctx, s) => {
      ctx.fillStyle = "#97948c";
      ctx.fillRect(0, 0, s, s);
      paintNoise(ctx, s, ["#8f8c84", "#a3a098", "#878680", "#9c9991"], 260, 12, 60, 0.5);
      paintNoise(ctx, s, ["#7b7872", "#6d6a65"], 110, 2, 7, 0.5); // pock marks
      paintNoise(ctx, s, ["#b0ada5", "#a8a59d"], 80, 1, 4, 0.55); // light chips

      // vertical grime streaks
      paintStreaks(ctx, s, {
        color: "#4d4b46",
        count: 14,
        minW: 6,
        maxW: 36,
        minLen: s,
        maxLen: s,
        minAlpha: 0.05,
        maxAlpha: 0.12,
        maxY: 0,
      });

      // expansion joints (tile seams)
      ctx.globalAlpha = 0.55;
      ctx.strokeStyle = "#5f5d58";
      ctx.lineWidth = 3;
      ctx.strokeRect(1, 1, s - 2, s - 2);

      // hairline cracks
      ctx.globalAlpha = 0.45;
      ctx.strokeStyle = "#67645f";
      ctx.lineWidth = 1;
      for (let i = 0; i < 7; i++) {
        ctx.beginPath();
        let x = Math.random() * s;
        let y = Math.random() * s;
        ctx.moveTo(x, y);
        for (let j = 0; j < 6; j++) {
          x += (Math.random() - 0.5) * 70;
          y += (Math.random() - 0.5) * 70;
          ctx.lineTo(x, y);
        }
        ctx.stroke();
      }
      ctx.globalAlpha = 1;
    });
  }

  // Industrial painted-steel panels with seams, rivets, scratches and rust
  public createMetalMaterial(): PBRMaterial {
    return canvasMat(this.scene, "metalPanelMat", 512, { rough: 0.5, metal: 0.55, wet: 0.55, bump: 1.6 }, (ctx, s) => {
      ctx.fillStyle = "#3d434b";
      ctx.fillRect(0, 0, s, s);
      paintNoise(ctx, s, ["#363c44", "#434a53", "#3a4049"], 200, 10, 50, 0.5);

      // panel seams (2x2 grid)
      ctx.globalAlpha = 0.85;
      ctx.strokeStyle = "#21252b";
      ctx.lineWidth = 4;
      for (const p of [0, s / 2, s]) {
        ctx.beginPath();
        ctx.moveTo(p, 0);
        ctx.lineTo(p, s);
        ctx.stroke();
        ctx.beginPath();
        ctx.moveTo(0, p);
        ctx.lineTo(s, p);
        ctx.stroke();
      }

      // rivets along seams
      ctx.globalAlpha = 0.9;
      for (const line of [8, s / 2 - 8, s / 2 + 8, s - 8]) {
        for (let i = 24; i < s; i += 48) {
          ctx.fillStyle = "#565e68";
          ctx.beginPath();
          ctx.arc(line, i, 3, 0, Math.PI * 2);
          ctx.fill();
          ctx.beginPath();
          ctx.arc(i, line, 3, 0, Math.PI * 2);
          ctx.fill();
        }
      }

      paintScratches(ctx, s, "#6f7782", 22, 10, 60, 0.35);

      // rust specks
      paintNoise(ctx, s, ["#6e4a2f", "#7d5436", "#5c3e28"], 60, 1, 5, 0.5);
      ctx.globalAlpha = 1;
    });
  }

  // Corrugation profile shared by the container albedo and its height map:
  // 1024 / 32 = 32 ribs evenly across the panel (zero seam clip), each a
  // trapezoid with a flat crown and sloped flanks like the real pressing.
  private static readonly RIB = 32;

  private static ribProfile(x: number): number {
    const t = (x % WorldMaterials.RIB) / WorldMaterials.RIB;
    if (t < 0.12) return t / 0.12; // rising flank
    if (t < 0.5) return 1; // crown
    if (t < 0.62) return 1 - (t - 0.5) / 0.12; // falling flank
    return 0; // valley
  }

  // Dedicated container height field + tangent normal map, built once per
  // scene and shared across all 5 container paint colors.
  private getContainerRelief(): { heightData: Float32Array; bumpTex: DynamicTexture } {
    if (!this.containerHeight) {
      const s = 1024;
      const h = new Float32Array(s * s);
      for (let y = 0; y < s; y++) {
        const rail = y < 22 || y > s - 22 ? 0.35 : 1;
        const row = y * s;
        for (let x = 0; x < s; x++) {
          h[row + x] = (0.35 + 0.65 * WorldMaterials.ribProfile(x)) * rail;
        }
      }
      // vertical weld seams every quarter panel
      for (const sx of [s * 0.25, s * 0.5, s * 0.75]) {
        for (let dx = -1; dx <= 1; dx++) {
          const x = sx + dx;
          for (let y = 0; y < s; y++) h[y * s + x] *= 0.5;
        }
      }
      // corner castings
      for (const [cx, cy] of [
        [0, 0],
        [s - 44, 0],
        [0, s - 44],
        [s - 44, s - 44],
      ]) {
        for (let y = cy; y < cy + 44; y++) {
          for (let x = cx; x < cx + 44; x++) h[y * s + x] = 232 / 255;
        }
      }
      this.containerHeight = h;
    }
    if (!this.containerBump) {
      this.containerBump =
        (this.scene.getTextureByName("containerTex_n") as DynamicTexture | null) ??
        normalMapFromHeight(this.scene, "containerTex_n", this.containerHeight, 1024, 2.2);
    }
    return { heightData: this.containerHeight, bumpTex: this.containerBump };
  }

  // Painted corrugated shipping-container steel, tinted per container:
  // shaded ribs, a weathered paint job with chips and rust runs, panel
  // seams, corner castings, and a stencilled owner code and placard
  public createContainerMaterial(key: ContainerColor): PBRMaterial {
    const { base, sideShade: shade } = CONTAINER_PALETTE[key];
    const { heightData, bumpTex } = this.getContainerRelief();
    return canvasMat(
      this.scene,
      `containerMat_${key}`,
      1024,
      { rough: 0.48, metal: 0.3, wet: 0.65, roughVar: 0.2, heightData, bumpTex, shared: this.containerWet },
      (ctx, s) => {
        ctx.fillStyle = base;
        ctx.fillRect(0, 0, s, s);

        // rib shading — light catches the crowns and the rising flanks
        const rib = WorldMaterials.RIB;
        for (let x = 0; x < s; x += rib) {
          const g = ctx.createLinearGradient(x, 0, x + rib, 0);
          g.addColorStop(0, "rgba(0,0,0,0.3)");
          g.addColorStop(0.12, "rgba(255,255,255,0.12)");
          g.addColorStop(0.5, "rgba(255,255,255,0.04)");
          g.addColorStop(0.62, "rgba(0,0,0,0.22)");
          g.addColorStop(1, "rgba(0,0,0,0.32)");
          ctx.fillStyle = g;
          ctx.fillRect(x, 0, rib, s);
        }

        // sun-faded and re-painted patches, general grime
        paintNoise(ctx, s, [shade], 160, 14, 70, 0.22);
        paintNoise(ctx, s, ["rgba(255,255,255,1)"], 40, 20, 90, 0.05);
        paintNoise(ctx, s, ["#2a2622", "#1e1c19"], 260, 1, 3, 0.35); // paint chips

        // rust: runs bleeding down from the top rail and the seams, blooms low where water sits
        paintStreaks(ctx, s, {
          color: "#7a4422",
          count: 18,
          minW: 2,
          maxW: 10,
          minLen: 60,
          maxLen: 340,
          minAlpha: 0.35,
          maxAlpha: 0.65,
          minY: 18,
          maxY: 48,
          fade: true,
        });
        for (const seamX of [s * 0.25, s * 0.5, s * 0.75]) {
          paintStreaks(ctx, s, {
            color: "#7a4422",
            count: 4,
            minW: 3,
            maxW: 8,
            minLen: 80,
            maxLen: 280,
            minAlpha: 0.25,
            maxAlpha: 0.35,
            minX: seamX - 4,
            maxX: seamX + 2,
            maxY: s * 0.6,
            fade: true,
          });
        }
        paintNoise(ctx, s, ["#6b3f22", "#7d4a28", "#53301a"], 90, 3, 12, 0.35);
        ctx.globalAlpha = 0.28;
        for (let i = 0; i < 12; i++) {
          const g = ctx.createRadialGradient(Math.random() * s, s - 40 - Math.random() * 120, 2, s * 0.5, s - 60, 160);
          g.addColorStop(0, "rgba(110,60,30,0.6)");
          g.addColorStop(1, "rgba(110,60,30,0)");
          ctx.fillStyle = g;
          ctx.fillRect(0, s - 220, s, 220);
        }
        ctx.globalAlpha = 1;

        // vertical weld seams between the pressed panels
        for (const x of [s * 0.25, s * 0.5, s * 0.75]) {
          ctx.fillStyle = "rgba(0,0,0,0.35)";
          ctx.fillRect(x - 2, 0, 4, s);
          ctx.fillStyle = "rgba(255,255,255,0.08)";
          ctx.fillRect(x + 2, 0, 1.5, s);
        }

        // owner code and serial, stencilled high on the panel, and a
        // placard low — the marks every box in a yard carries
        const codes = ["MRDN", "TQLU", "HGSU", "BSLU", "CRXU", "PKGU"];
        const code = codes[(key.length * 7 + key.charCodeAt(0)) % codes.length];
        const serial = `${((key.charCodeAt(0) * 31) % 900) + 100} ${((key.length * 173) % 900) + 100} ${key.charCodeAt(key.length - 1) % 10}`;
        ctx.fillStyle = "rgba(240,240,236,0.82)";
        ctx.font = `700 ${Math.round(s * 0.052)}px "Arial Narrow", Arial, sans-serif`;
        ctx.textBaseline = "top";
        ctx.fillText(code, s * 0.06, s * 0.08);
        ctx.fillText(serial, s * 0.06, s * 0.14);
        ctx.font = `700 ${Math.round(s * 0.028)}px Arial, sans-serif`;
        ctx.fillText("22G1", s * 0.06, s * 0.2);
        ctx.fillText("MAX GROSS 30 480 KG · TARE 2 250 KG", s * 0.06, s * 0.86);
        // placard frame
        ctx.strokeStyle = "rgba(240,240,236,0.5)";
        ctx.lineWidth = 3;
        ctx.strokeRect(s * 0.72, s * 0.76, s * 0.2, s * 0.14);
        ctx.fillStyle = "rgba(240,240,236,0.6)";
        ctx.font = `700 ${Math.round(s * 0.024)}px Arial, sans-serif`;
        ctx.fillText("CSC SAFETY", s * 0.735, s * 0.775);
        ctx.fillText("APPROVAL", s * 0.735, s * 0.805);
        ctx.fillText("GB/L/1874/03", s * 0.735, s * 0.845);
        // grime over the lettering so it reads as old paint, not a decal
        paintNoise(ctx, s, [shade], 40, 10, 40, 0.18);

        // top/bottom frame rails and corner castings
        ctx.fillStyle = "rgba(0,0,0,0.5)";
        ctx.fillRect(0, 0, s, 22);
        ctx.fillRect(0, s - 22, s, 22);
        ctx.fillStyle = "#3a3a3c";
        for (const [x, y] of [
          [0, 0],
          [s - 44, 0],
          [0, s - 44],
          [s - 44, s - 44],
        ]) {
          ctx.fillRect(x, y, 44, 44);
          ctx.fillStyle = "#1c1c1e";
          ctx.fillRect(x + 12, y + 12, 20, 20); // the casting's oval hole
          ctx.fillStyle = "#3a3a3c";
        }
      }
    );
  }

  // Rough plank wood for crates and tower steps
  public createWoodCrateMaterial(): PBRMaterial {
    return canvasMat(this.scene, "woodCrateMat", 512, { rough: 0.8, wet: 0.3, bump: 1.6 }, (ctx, s) => {
      ctx.fillStyle = "#8f6f48";
      ctx.fillRect(0, 0, s, s);

      // horizontal planks with seams and grain
      for (let y = 0; y < s; y += 52) {
        ctx.fillStyle = `rgba(${60 + Math.random() * 30}, ${40 + Math.random() * 20}, ${20 + Math.random() * 12}, 0.25)`;
        ctx.fillRect(0, y, s, 52);
        ctx.fillStyle = "rgba(40, 26, 14, 0.8)";
        ctx.fillRect(0, y, s, 3);
        // grain strokes
        ctx.strokeStyle = "rgba(70, 50, 28, 0.35)";
        ctx.lineWidth = 1;
        for (let i = 0; i < 5; i++) {
          const gy = y + 8 + Math.random() * 38;
          ctx.beginPath();
          ctx.moveTo(0, gy);
          ctx.bezierCurveTo(s * 0.3, gy + (Math.random() - 0.5) * 8, s * 0.7, gy + (Math.random() - 0.5) * 8, s, gy);
          ctx.stroke();
        }
      }
      // knots
      paintNoise(ctx, s, ["#5c3f24", "#4e3520"], 8, 2, 5, 0.7);
      // crate frame border
      ctx.strokeStyle = "rgba(48, 32, 16, 0.85)";
      ctx.lineWidth = 14;
      ctx.strokeRect(7, 7, s - 14, s - 14);
    });
  }

  public createGrassMaterial(uScale: number = 10, vScale: number = 10): PBRMaterial {
    return canvasMat(
      this.scene,
      `grassMat_${uScale}_${vScale}`,
      512,
      { rough: 0.78, wet: 0.85, roughVar: 0.2, bump: 0.7, u: uScale, v: vScale },
      (ctx, s) => {
        // wet sheen
        ctx.fillStyle = "#42523a";
        ctx.fillRect(0, 0, s, s);
        paintNoise(ctx, s, ["#3a4a34", "#48583e", "#37452f", "#4d5c42"], 320, 8, 42, 0.5);
        paintNoise(ctx, s, ["#2e3c2a", "#334030"], 200, 2, 6, 0.4); // shadow clumps

        // blade flecks — short leaning strokes
        for (let i = 0; i < 900; i++) {
          ctx.globalAlpha = 0.22 + Math.random() * 0.3;
          ctx.strokeStyle = ["#55654a", "#4a5a40", "#5e6c50", "#3f4f36"][(Math.random() * 4) | 0];
          ctx.lineWidth = 1;
          const x = Math.random() * s;
          const y = Math.random() * s;
          ctx.beginPath();
          ctx.moveTo(x, y);
          ctx.lineTo(x + (Math.random() - 0.5) * 3, y - (2 + Math.random() * 5));
          ctx.stroke();
        }

        // mud worn through + standing water
        paintNoise(ctx, s, ["#4a4136", "#3e362c", "#52483a"], 24, 6, 22, 0.3);
        paintNoise(ctx, s, ["#2c352e", "#28302c"], 16, 10, 30, 0.28);
        ctx.globalAlpha = 1;
      }
    );
  }

  // Flagstone lay for the walkways: an irregular grid of flat polygonal
  // slabs with tight dark joints, each slab its own tone, worn corners and
  // moss in the gaps. The height field is flat slabs over sunken joints so
  // the relief is edges, not domes.
  private stoneLayout: Array<{ pts: Array<[number, number]>; tone: number; wear: number }> | null = null;

  private getStoneLayout(s: number): Array<{ pts: Array<[number, number]>; tone: number; wear: number }> {
    if (this.stoneLayout) return this.stoneLayout;
    const cols = 6;
    const cell = s / cols;
    const jitter = cell * 0.22;
    // jittered lattice corners shared between neighbours so slabs interlock
    const corner = (i: number, j: number): [number, number] => {
      const seedX = ((i * 73 + j * 151) % 97) / 97;
      const seedZ = ((i * 31 + j * 17) % 89) / 89;
      return [i * cell + (seedX - 0.5) * jitter, j * cell + (seedZ - 0.5) * jitter];
    };
    const slabs: Array<{ pts: Array<[number, number]>; tone: number; wear: number }> = [];
    for (let j = 0; j < cols; j++) {
      for (let i = 0; i < cols; i++) {
        const stagger = j % 2 ? 0.5 : 0; // running bond
        const pts: Array<[number, number]> = [
          corner(i + stagger, j),
          corner(i + 1 + stagger, j),
          corner(i + 1 + stagger, j + 1),
          corner(i + stagger, j + 1),
        ];
        slabs.push({ pts, tone: (i * 7 + j * 3) % 6, wear: ((i * 13 + j * 29) % 10) / 10 });
      }
    }
    this.stoneLayout = slabs;
    return slabs;
  }

  private static tracePoly(
    ctx: CanvasRenderingContext2D,
    pts: Array<[number, number]>,
    ox: number,
    oy: number,
    inset: number
  ): void {
    // shrink toward the centroid for the joint gap
    const cx = pts.reduce((a, p) => a + p[0], 0) / pts.length;
    const cy = pts.reduce((a, p) => a + p[1], 0) / pts.length;
    ctx.beginPath();
    pts.forEach(([x, y], k) => {
      const dx = x - cx;
      const dy = y - cy;
      const len = Math.hypot(dx, dy) || 1;
      const px = x - (dx / len) * inset + ox;
      const py = y - (dy / len) * inset + oy;
      if (k === 0) ctx.moveTo(px, py);
      else ctx.lineTo(px, py);
    });
    ctx.closePath();
  }

  public createStoneWalkwayMaterial(): PBRMaterial {
    const slabs = this.getStoneLayout(512);
    const wrapOffsets: Array<[number, number]> = [
      [-512, -512],
      [0, -512],
      [512, -512],
      [-512, 0],
      [0, 0],
      [512, 0],
      [-512, 512],
      [0, 512],
      [512, 512],
    ];
    return canvasMat(
      this.scene,
      "stoneWalkMat",
      512,
      {
        rough: 0.62,
        wet: 0.8,
        roughVar: 0.2,
        bump: 1.6,
        height: (ctx, s) => {
          ctx.fillStyle = "#2a2a2a"; // joints sit low
          ctx.fillRect(0, 0, s, s);
          for (const slab of slabs) {
            const lift = 200 + slab.wear * 40; // slabs sit a little unevenly
            ctx.fillStyle = `rgb(${lift},${lift},${lift})`;
            for (const [ox, oy] of wrapOffsets) {
              WorldMaterials.tracePoly(ctx, slab.pts, ox, oy, 5);
              ctx.fill();
            }
          }
          paintNoise(ctx, s, ["#9a9a9a", "#b8b8b8"], 400, 1, 3, 0.35); // pitting
        },
      },
      (ctx, s) => {
        ctx.fillStyle = "#2f2e2b"; // wet mortar
        ctx.fillRect(0, 0, s, s);
        paintNoise(ctx, s, ["#3a3a36", "#282825"], 120, 4, 16, 0.4);
        const tones = ["#6e6c66", "#63625c", "#77756d", "#5c5b55", "#6a675f", "#615e57"];
        for (const slab of slabs) {
          for (const [ox, oy] of wrapOffsets) {
            WorldMaterials.tracePoly(ctx, slab.pts, ox, oy, 5);
            ctx.fillStyle = tones[slab.tone];
            ctx.fill();
            // a darker rim where the worn edge falls away toward the joint
            ctx.strokeStyle = "rgba(30,30,28,0.45)";
            ctx.lineWidth = 3;
            ctx.stroke();
          }
        }
        // grime, chips, wet glints and moss in the joints
        paintNoise(ctx, s, ["#4a4944", "#3e3d39"], 180, 1, 5, 0.35);
        paintNoise(ctx, s, ["#8d8b82", "#96948b"], 70, 1, 3, 0.3);
        paintNoise(ctx, s, ["#46503a", "#3e4834"], 50, 2, 7, 0.3);
      }
    );
  }

  public createGrassBladeTexture(): RawTexture {
    const s = 512;
    const canvas = document.createElement("canvas");
    canvas.width = canvas.height = s;
    const ctx = canvas.getContext("2d", { willReadFrequently: true })!;
    const tones = [
      ["#1f2b1a", "#3d4f32", "#6b7d4e"],
      ["#22301d", "#455739", "#748558"],
      ["#1c2718", "#394a2f", "#647748"],
      ["#26331f", "#4a5c3d", "#7b8b5c"],
    ];
    ctx.strokeStyle = "#9aab74";
    ctx.lineWidth = 1.2;
    for (let i = 0; i < 17; i++) {
      const tall = Math.random() < 0.45;
      const spread = i / 16 - 0.5;
      const x = s * (0.5 + spread * 0.7 + (Math.random() - 0.5) * 0.05);
      const width = s * (0.025 + Math.random() * 0.022);
      const tipX = x + s * (spread * 0.24 + (Math.random() - 0.5) * 0.22);
      const tipY = s * ((tall ? 0.08 : 0.33) + Math.random() * 0.25);
      const [lo, mid, hi] = tones[(Math.random() * 4) | 0];
      const elbow = x + (tipX - x) * 0.26;
      const g = ctx.createLinearGradient(0, s, 0, tipY);
      g.addColorStop(0, lo);
      g.addColorStop(0.55, mid);
      g.addColorStop(1, hi);
      ctx.fillStyle = g;
      ctx.beginPath();
      ctx.moveTo(x - width / 2, s);
      ctx.quadraticCurveTo(elbow - width, s * 0.49, tipX, tipY);
      ctx.quadraticCurveTo(elbow + width, s * 0.57, x + width / 2, s);
      ctx.fill();
      ctx.globalAlpha = 0.25;
      ctx.beginPath();
      ctx.moveTo(x, s);
      ctx.quadraticCurveTo(elbow, s * 0.55, tipX, tipY + 12);
      ctx.stroke();
      ctx.globalAlpha = 1;
    }
    // Fill transparent texels with grass green in the raw upload. Canvas
    // would premultiply them to black and darken the filtered blade edges.
    const rgba = new Uint8Array(ctx.getImageData(0, 0, s, s).data.buffer);
    for (let i = 0; i < rgba.length; i += 4) {
      if (rgba[i + 3]) continue;
      rgba[i] = 42;
      rgba[i + 1] = 56;
      rgba[i + 2] = 36;
    }
    const tex = RawTexture.CreateRGBATexture(rgba, s, s, this.scene, true, true);
    tex.name = "grassBladeTex";
    tex.hasAlpha = true;
    tex.anisotropicFilteringLevel = 8;
    return tex;
  }

  // Soft vertical streak for the rain particles (transparent background)
  public createRainStreakTexture(): DynamicTexture {
    const cached = this.scene.getTextureByName("rainStreakTex");
    if (cached) return cached as DynamicTexture;

    const tex = makeCanvasTexture(this.scene, "rainStreakTex", 64, (ctx, s) => {
      ctx.clearRect(0, 0, s, s);
      const g = ctx.createLinearGradient(0, 0, 0, s);
      g.addColorStop(0, "rgba(215,228,240,0)");
      g.addColorStop(0.35, "rgba(215,228,240,0.5)");
      g.addColorStop(0.65, "rgba(225,236,246,0.85)");
      g.addColorStop(1, "rgba(215,228,240,0)");
      ctx.fillStyle = g;
      ctx.fillRect(s * 0.42, 0, s * 0.16, s);
    });
    tex.hasAlpha = true;
    return tex;
  }

  private getContainerDoorRelief(): { heightData: Float32Array; bumpTex: DynamicTexture } {
    if (!this.containerDoorHeight) {
      const s = 512;
      const h = new Float32Array(s * s);
      const fill = (x0: number, y0: number, w: number, hh: number, val: number): void => {
        const x1 = Math.min(s, Math.max(0, Math.round(x0 + w)));
        const y1 = Math.min(s, Math.max(0, Math.round(y0 + hh)));
        const xs = Math.max(0, Math.round(x0));
        const ys = Math.max(0, Math.round(y0));
        for (let y = ys; y < y1; y++) h.fill(val, y * s + xs, y * s + x1);
      };
      for (let y = 0; y < s; y++) {
        const t = (y % 22) / 22;
        const wave = t < 0.4 ? 0.35 + (t / 0.4) * 0.3 : 0.65 - ((t - 0.4) / 0.6) * 0.3;
        h.fill(wave, y * s, (y + 1) * s);
      }
      fill(s / 2 - 2, 0, 4, s, 0.2); // center seam
      for (const fx of [0.16, 0.4, 0.6, 0.84]) {
        const x = fx * s;
        fill(x - 3, 8, 6, s - 16, 0.82); // lock rod
        for (let y = 30; y < s - 20; y += 60) fill(x - 6, y, 12, 10, 0.92); // brackets
        fill(fx < 0.5 ? x - 3 : x - 23, s * 0.6, 26, 7, 0.88); // handles
      }
      fill(0, 0, s, 10, 0.25);
      fill(0, s - 10, s, 10, 0.25);
      fill(0, 0, 8, s, 0.25);
      fill(s - 8, 0, 8, s, 0.25);
      this.containerDoorHeight = h;
    }
    if (!this.containerDoorBump) {
      this.containerDoorBump =
        (this.scene.getTextureByName("containerDoorTex_n") as DynamicTexture | null) ??
        normalMapFromHeight(this.scene, "containerDoorTex_n", this.containerDoorHeight, 512, 2.0);
    }
    return { heightData: this.containerDoorHeight, bumpTex: this.containerDoorBump };
  }

  public createContainerDoorMaterial(key: ContainerColor): PBRMaterial {
    const { base, doorShade: shade } = CONTAINER_PALETTE[key];
    const { heightData, bumpTex } = this.getContainerDoorRelief();
    return canvasMat(
      this.scene,
      `containerDoorMat_${key}`,
      512,
      { rough: 0.5, metal: 0.3, wet: 0.65, heightData, bumpTex, shared: this.containerDoorWet },
      (ctx, s) => {
        ctx.fillStyle = shade;
        ctx.fillRect(0, 0, s, s);

        // shallow horizontal door corrugation
        for (let y = 0; y < s; y += 22) {
          const g = ctx.createLinearGradient(0, y, 0, y + 22);
          g.addColorStop(0, "rgba(0,0,0,0.22)");
          g.addColorStop(0.4, "rgba(255,255,255,0.07)");
          g.addColorStop(1, "rgba(0,0,0,0.24)");
          ctx.fillStyle = g;
          ctx.fillRect(0, y, s, 22);
        }
        paintNoise(ctx, s, [base], 60, 6, 26, 0.18);

        // center seam between the two door leaves
        ctx.globalAlpha = 0.8;
        ctx.fillStyle = "rgba(0,0,0,0.55)";
        ctx.fillRect(s / 2 - 2, 0, 4, s);

        // four vertical lock rods with keeper brackets
        for (const fx of [0.16, 0.4, 0.6, 0.84]) {
          const x = fx * s;
          ctx.globalAlpha = 0.95;
          ctx.fillStyle = "#9aa0a4";
          ctx.fillRect(x - 3, 8, 6, s - 16);
          ctx.fillStyle = "#5d6367";
          ctx.fillRect(x - 2, 8, 2, s - 16);
          ctx.fillStyle = "#74797d";
          for (let y = 30; y < s - 20; y += 60) {
            ctx.fillRect(x - 6, y, 12, 10); // brackets
          }
          // handle bars at waist height
          ctx.fillStyle = "#8b9094";
          ctx.fillRect(x - 3, s * 0.6, fx < 0.5 ? 26 : -20, 7);
        }

        // shipping placard, top right
        ctx.globalAlpha = 0.85;
        ctx.fillStyle = "#cfd3cd";
        ctx.fillRect(s * 0.66, 18, 44, 30);
        ctx.fillStyle = "#41464a";
        ctx.fillRect(s * 0.66 + 5, 24, 34, 4);
        ctx.fillRect(s * 0.66 + 5, 32, 26, 3);
        ctx.fillRect(s * 0.66 + 5, 39, 30, 3);

        // rust bleeding off the hardware
        paintStreaks(ctx, s, {
          color: "#6b4226",
          count: 8,
          minW: 3,
          maxW: 9,
          minLen: 30,
          maxLen: 120,
          minAlpha: 0.1,
          maxAlpha: 0.24,
          maxY: s * 0.4,
        });

        // frame rails
        ctx.globalAlpha = 0.7;
        ctx.fillStyle = "rgba(0,0,0,0.5)";
        ctx.fillRect(0, 0, s, 10);
        ctx.fillRect(0, s - 10, s, 10);
        ctx.fillRect(0, 0, 8, s);
        ctx.fillRect(s - 8, 0, 8, s);
        ctx.globalAlpha = 1;
      }
    );
  }

  // Industrial window band for the out-of-bounds warehouse facades
  public createWindowBandMaterial(): PBRMaterial {
    return canvasMat(this.scene, "windowBandMat", 256, { rough: 0.3, metal: 0.2, u: 8, v: 1 }, (ctx, s) => {
      ctx.fillStyle = "#252b31";
      ctx.fillRect(0, 0, s, s);
      // panes with a faint sky-reflection gradient, some broken/dark
      for (let x = 0; x < s; x += 32) {
        for (let y = 0; y < s; y += 64) {
          const g = ctx.createLinearGradient(0, y, 0, y + 64);
          const broken = Math.random() < 0.18;
          g.addColorStop(0, broken ? "#1b1f23" : "#5a656d");
          g.addColorStop(1, broken ? "#15181b" : "#39424a");
          ctx.fillStyle = g;
          ctx.fillRect(x + 2, y + 3, 28, 58);
        }
      }
      // mullions
      ctx.fillStyle = "#8e979d";
      for (let x = 0; x <= s; x += 32) ctx.fillRect(x - 1, 0, 3, s);
      ctx.fillRect(0, s / 2 - 2, s, 4);
      paintNoise(ctx, s, ["#23282c"], 40, 2, 8, 0.25);
    });
  }

  // Weathered factory paint for the abandoned car: faded petrol blue with
  // chipped edges, rust freckles and rain-streak grime. Panel gaps belong
  // to the geometry so they don't repeat across the hood, roof and trunk.
  public createCarBodyMaterial(): PBRMaterial {
    return canvasMat(this.scene, "carBodyMat", 512, { rough: 0.32, metal: 0.55, wet: 0.5, bump: 0.9 }, (ctx, s) => {
      // wet clear-coat glint
      ctx.fillStyle = "#3f5560";
      ctx.fillRect(0, 0, s, s);
      paintNoise(ctx, s, ["#445a66", "#3a4f59", "#48606b", "#374a53"], 150, 8, 30, 0.4);

      // clear-coat sheen band along the shoulder line
      const sheen = ctx.createLinearGradient(0, 0, 0, s);
      sheen.addColorStop(0, "rgba(255,255,255,0.10)");
      sheen.addColorStop(0.35, "rgba(255,255,255,0.02)");
      sheen.addColorStop(1, "rgba(0,0,0,0.16)");
      ctx.fillStyle = sheen;
      ctx.fillRect(0, 0, s, s);

      // rust freckles concentrated low + chipped bright primer specks
      paintNoise(ctx, s, ["#6e4a2f", "#5c3e28"], 50, 1, 4, 0.45);
      paintNoise(ctx, s, ["#8da0a8", "#9fb1b8"], 30, 1, 2, 0.4);

      // rain-streak grime running down
      paintStreaks(ctx, s, {
        color: "#1f2c33",
        count: 12,
        minW: 3,
        maxW: 11,
        minLen: 40,
        maxLen: 160,
        minAlpha: 0.06,
        maxAlpha: 0.14,
        maxY: s * 0.3,
      });
    });
  }

  // Overcast sky: a low cloud deck with real structure — layered soft
  // blotches at three scales over the gradient, darker undersides, a
  // brighter horizon where the light leaks in under the cloud base
  // Overcast: a stratus deck built from 3D fractal noise evaluated on the
  // sphere's direction vectors — seamless around the dome, no pinch at the
  // zenith, low contrast like a real grey day. The deck brightens toward
  // the horizon where the light leaks in under it, thins near a soft sun
  // patch, and carries a slow large-scale drift of darker rain cells.
  public createSkyMaterial(): StandardMaterial {
    const S = 512;
    // hashed 3D value noise
    const hash = (x: number, y: number, z: number): number => {
      let h = (x * 374761393 + y * 668265263 + z * 1274126177) | 0;
      h = ((h ^ (h >>> 13)) * 1274126177) | 0;
      return ((h ^ (h >>> 16)) >>> 0) / 4294967295;
    };
    const smooth = (t: number): number => t * t * (3 - 2 * t);
    const noise = (x: number, y: number, z: number): number => {
      const xi = Math.floor(x);
      const yi = Math.floor(y);
      const zi = Math.floor(z);
      const fx = smooth(x - xi);
      const fy = smooth(y - yi);
      const fz = smooth(z - zi);
      const lerp = (a: number, b: number, t: number): number => a + (b - a) * t;
      const c00 = lerp(hash(xi, yi, zi), hash(xi + 1, yi, zi), fx);
      const c10 = lerp(hash(xi, yi + 1, zi), hash(xi + 1, yi + 1, zi), fx);
      const c01 = lerp(hash(xi, yi, zi + 1), hash(xi + 1, yi, zi + 1), fx);
      const c11 = lerp(hash(xi, yi + 1, zi + 1), hash(xi + 1, yi + 1, zi + 1), fx);
      return lerp(lerp(c00, c10, fy), lerp(c01, c11, fy), fz);
    };
    const fbm = (x: number, y: number, z: number, octaves: number): number => {
      let sum = 0;
      let amp = 0.5;
      let f = 1;
      for (let i = 0; i < octaves; i++) {
        sum += amp * noise(x * f + 11.3 * i, y * f + 7.1 * i, z * f + 3.7 * i);
        amp *= 0.5;
        f *= 2.1;
      }
      return sum;
    };

    const tex = new DynamicTexture("skyTex", { width: S, height: S }, this.scene, true);
    const ctx = tex.getContext() as CanvasRenderingContext2D;
    const img = ctx.createImageData(S, S);
    const d = img.data;
    // palette: cloud base, lit cloud, horizon haze, rain cell
    const base = [96, 102, 110];
    const lit = [168, 174, 180];
    const haze = [186, 190, 194];
    const rain = [70, 76, 84];
    const sunDir = [0.62, 0.34, 0.71]; // low, off to one side
    for (let row = 0; row < S; row++) {
      // canvas is uploaded flipped: row S is the zenith, row S/2 the horizon
      const v = row / S;
      const elevation = (v - 0.5) * Math.PI; // -90° (nadir) .. +90° (zenith)
      const ce = Math.cos(elevation);
      const se = Math.sin(elevation);
      for (let col = 0; col < S; col++) {
        const az = (col / S) * Math.PI * 2;
        const dx = Math.sin(az) * ce;
        const dy = se;
        const dz = Math.cos(az) * ce;
        // cloud deck lives on a flat plane above the viewer: project the ray
        // onto it so the texture reads as an overhead layer, not a painted ball
        const h = Math.max(0.08, dy);
        const px = (dx / h) * 1.6;
        const pz = (dz / h) * 1.6;
        const fade = Math.min(1, Math.max(0, (dy - 0.02) / 0.35)); // haze swallows the deck at the horizon
        const n = fbm(px, pz, 0.7, 5); // 0..~1
        const cells = fbm(px * 0.35 + 40, pz * 0.35 + 40, 2.1, 3); // big dark rain cells
        const dot = dx * sunDir[0] + dy * sunDir[1] + dz * sunDir[2];
        const sun = Math.pow(Math.max(0, dot), 6) * 0.55; // broad glow through the deck
        let t = n * 0.85 + sun - (cells - 0.5) * 0.5; // lit vs base mix
        t = Math.min(1, Math.max(0, t));
        const i = (row * S + col) * 4;
        for (let c = 0; c < 3; c++) {
          let deck = base[c] + (lit[c] - base[c]) * t;
          deck = deck + (rain[c] - deck) * Math.max(0, cells - 0.62) * 1.4;
          d[i + c] = deck + (haze[c] - deck) * (1 - fade);
        }
        d[i + 3] = 255;
      }
    }
    ctx.putImageData(img, 0, 0);
    tex.update();

    const mat = new StandardMaterial("skyMat", this.scene);
    mat.emissiveTexture = tex;
    mat.diffuseColor = Color3.Black();
    mat.specularColor = Color3.Black();
    mat.disableLighting = true;
    mat.backFaceCulling = false;
    mat.fogEnabled = false;
    return mat;
  }

  // Four wall strips share one atlas. Each strip has four Buddha quotations
  // rather than repeating the same square stamp six times along every wall.
  // The long faces choose a strip in ShipBoxMap; the small faces sample its
  // quiet left margin. 8px vertical gutters keep neighbouring walls out of
  // the filtered UVs. A 2MP albedo plus tiny concrete normal costs less than
  // the old three 1MP maps, and all four wall meshes can still merge.
  public createGraffitiWallMaterial(): PBRMaterial {
    const name = "graffitiWallAtlasMat";
    const cached = this.scene.getMaterialByName(name);
    if (cached) return cached as PBRMaterial;

    const tex = new DynamicTexture("graffitiWallAtlasTex", { width: 2048, height: 1024 }, this.scene, true);
    tex.wrapU = Texture.CLAMP_ADDRESSMODE;
    tex.wrapV = Texture.CLAMP_ADDRESSMODE;
    tex.anisotropicFilteringLevel = 8;
    paintGraffitiAtlas(tex.getContext() as CanvasRenderingContext2D);
    tex.update();

    // Concrete grain alone supplies relief. Deriving normals from the
    // coloured artwork made pale outlines swell and dark ink look carved.
    const normalSize = 256;
    const height = new Float32Array(normalSize * normalSize);
    for (let y = 0; y < normalSize; y++) {
      for (let x = 0; x < normalSize; x++) {
        const grain = Math.sin(x * 127.1 + y * 311.7) * 43758.5453;
        height[y * normalSize + x] = 0.5 + (grain - Math.floor(grain) - 0.5) * 0.045;
      }
    }
    const bump = normalMapFromHeight(this.scene, "graffitiWallConcreteTex_n", height, normalSize, 1.0);
    return flatMat(this.scene, name, { albedo: [1, 1, 1], rough: 0.87, tex, bump });
  }

  // Hoarding posters — original artwork painted here (no photos, nothing to
  // license): a freight-line advert and a dockside radio poster, weathered
  // in place so the print reads as pasted up years ago, not bolted in fresh.
  public createBillboardMuralMaterial(id: 1 | 2): PBRMaterial {
    const name = `billboardMuralMat_${id}`;
    const cached = this.scene.getMaterialByName(name);
    if (cached) return cached as PBRMaterial;

    const w = id === 1 ? 768 : 512;
    const h = 1024;
    const tex = new DynamicTexture(`billboardMuralTex_${id}`, { width: w, height: h }, this.scene, true);
    tex.anisotropicFilteringLevel = 8;
    const ctx = tex.getContext() as CanvasRenderingContext2D;

    if (id === 1) paintFreightPoster(ctx, w, h);
    else paintRadioPoster(ctx, w, h);
    weatherPoster(ctx, w, h);
    tex.update();

    // Pasted paper: matte, a little sheen where the rain has hit it
    return flatMat(this.scene, name, { albedo: [0.7, 0.71, 0.73], rough: 0.78, tex });
  }
}
