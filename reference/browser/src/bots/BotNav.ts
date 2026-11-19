import type { Vector3 } from "@babylonjs/core";
import { PlayerController } from "../player/PlayerController";

// Spatial intelligence for the bots, derived entirely from the SAME oriented
// collision boxes the player collides with (PlayerController's registry):
//   - line-of-sight / bullet occlusion (3D segment-vs-OBB slab tests)
//   - a walkable waypoint grid + A*, built once at startup
// One source of truth: if a prop blocks the player, it blocks bot sight and
// bot pathing identically. No navmesh asset, no hand-traced graph to rot
// when the map changes — rebuilding the level rebuilds the bots' brains.

interface NavBox {
  cx: number;
  cz: number;
  hw: number;
  hd: number;
  minY: number;
  maxY: number;
  cos: number;
  sin: number;
}

// 1.2m spacing matters: the NS lane narrows to ~1m between the NW block and
// the center barrels, and a coarser grid drops every node in that throat —
// severing the center cross from the graph and exiling bots to the ring
const GRID_STEP = 1.2;
const GRID_HALF = 15.0; // node band inside the walls
const BODY_RADIUS = 0.42; // bot capsule + a sliver of skin for pathing
const WALK_BAND_LO = 0.15; // obstacles overlapping this y-band block walking...
const WALK_BAND_HI = 1.75; // ...anything fully above it (container roofs) does not
const COVER_MIN_HEIGHT = 1.2; // adjacent blockers at least this tall count as cover

const CELL_MIN = -16;
const CELL_MAX = 16;
const CELL_GRID = 8;
const CELL_SIZE = (CELL_MAX - CELL_MIN) / CELL_GRID; // 4m per cell

class BotNavSystem {
  private boxes: NavBox[] = [];
  private grid: number[][] = [];
  private outerBoxes: number[] = [];
  private boxStamp = new Int32Array(0);
  private queryId = 0;

  // Grid nodes (column-major: index = ix * side + iz)
  private side = 0;
  public count = 0;
  public xs!: Float32Array;
  public zs!: Float32Array;
  public walkable!: Uint8Array;
  public cover!: Uint8Array; // node sits beside something tall enough to duck behind
  private edges: number[][] = [];
  private edgeCosts: number[][] = [];

  // A* scratch, allocated once
  private g!: Float32Array;
  private f!: Float32Array;
  private came!: Int32Array;
  private state!: Uint8Array; // 0 unseen, 1 open, 2 closed
  private heap!: Int32Array;
  private heapPos!: Int32Array;
  private heapSize = 0;

  // Snapshot the obstacle registry and grow the waypoint graph over it.
  // Call once, after the map has registered all of its collision boxes.
  public build(): void {
    this.boxes = PlayerController.getObstacles().map((o) => ({
      cx: o.cx,
      cz: o.cz,
      hw: o.hw,
      hd: o.hd,
      minY: o.minY,
      maxY: o.maxY,
      cos: Math.cos(o.yaw),
      sin: Math.sin(o.yaw),
    }));

    this.grid = Array.from({ length: CELL_GRID * CELL_GRID }, () => []);
    this.outerBoxes = [];
    this.boxStamp = new Int32Array(this.boxes.length);
    this.queryId = 0;
    for (let bi = 0; bi < this.boxes.length; bi++) {
      const b = this.boxes[bi];
      const extX = Math.abs(b.cos) * b.hw + Math.abs(b.sin) * b.hd;
      const extZ = Math.abs(b.sin) * b.hw + Math.abs(b.cos) * b.hd;
      const minX = b.cx - extX;
      const maxX = b.cx + extX;
      const minZ = b.cz - extZ;
      const maxZ = b.cz + extZ;
      if (minX < CELL_MIN || maxX > CELL_MAX || minZ < CELL_MIN || maxZ > CELL_MAX) {
        this.outerBoxes.push(bi);
      }
      if (maxX >= CELL_MIN && minX <= CELL_MAX && maxZ >= CELL_MIN && minZ <= CELL_MAX) {
        const cx0 = Math.max(0, Math.min(CELL_GRID - 1, Math.floor((minX - CELL_MIN) / CELL_SIZE)));
        const cx1 = Math.max(0, Math.min(CELL_GRID - 1, Math.floor((maxX - CELL_MIN) / CELL_SIZE)));
        const cz0 = Math.max(0, Math.min(CELL_GRID - 1, Math.floor((minZ - CELL_MIN) / CELL_SIZE)));
        const cz1 = Math.max(0, Math.min(CELL_GRID - 1, Math.floor((maxZ - CELL_MIN) / CELL_SIZE)));
        for (let ix = cx0; ix <= cx1; ix++) {
          for (let iz = cz0; iz <= cz1; iz++) {
            this.grid[ix * CELL_GRID + iz].push(bi);
          }
        }
      }
    }

    this.side = Math.round((GRID_HALF * 2) / GRID_STEP) + 1;
    this.count = this.side * this.side;
    this.xs = new Float32Array(this.count);
    this.zs = new Float32Array(this.count);
    this.walkable = new Uint8Array(this.count);
    this.cover = new Uint8Array(this.count);
    this.edges = new Array(this.count);
    this.edgeCosts = new Array(this.count);
    this.g = new Float32Array(this.count);
    this.f = new Float32Array(this.count);
    this.came = new Int32Array(this.count);
    this.state = new Uint8Array(this.count);
    this.heap = new Int32Array(this.count);
    this.heapPos = new Int32Array(this.count);

    for (let ix = 0; ix < this.side; ix++) {
      for (let iz = 0; iz < this.side; iz++) {
        const i = ix * this.side + iz;
        this.xs[i] = -GRID_HALF + ix * GRID_STEP;
        this.zs[i] = -GRID_HALF + iz * GRID_STEP;
        this.walkable[i] = this.walkBlocked(this.xs[i], this.zs[i], this.xs[i], this.zs[i]) ? 0 : 1;
      }
    }

    const diagStep = GRID_STEP * Math.SQRT2;
    for (let ix = 0; ix < this.side; ix++) {
      for (let iz = 0; iz < this.side; iz++) {
        const i = ix * this.side + iz;
        const list: number[] = [];
        const costs: number[] = [];
        let coverAdj = false;
        for (let dx = -1; dx <= 1; dx++) {
          for (let dz = -1; dz <= 1; dz++) {
            if (dx === 0 && dz === 0) continue;
            const nx = ix + dx;
            const nz = iz + dz;
            if (nx < 0 || nz < 0 || nx >= this.side || nz >= this.side) continue;
            const n = nx * this.side + nz;
            if (this.walkable[i] && this.walkable[n] && !this.walkBlocked(this.xs[i], this.zs[i], this.xs[n], this.zs[n])) {
              list.push(n);
              costs.push(dx !== 0 && dz !== 0 ? diagStep : GRID_STEP);
            }
            if (!this.walkable[n] && this.tallAt(this.xs[n], this.zs[n])) {
              coverAdj = true;
            }
          }
        }
        this.edges[i] = list;
        this.edgeCosts[i] = costs;
        this.cover[i] = this.walkable[i] && coverAdj ? 1 : 0;
      }
    }
  }

  // Is there a chest-high blocker at this spot? (cover detection)
  private tallAt(x: number, z: number): boolean {
    for (const b of this.boxes) {
      if (b.maxY < COVER_MIN_HEIGHT || b.minY > 0.6) continue;
      const dx = x - b.cx;
      const dz = z - b.cz;
      const lx = b.cos * dx - b.sin * dz;
      const lz = b.sin * dx + b.cos * dz;
      if (Math.abs(lx) < b.hw + BODY_RADIUS && Math.abs(lz) < b.hd + BODY_RADIUS) return true;
    }
    return false;
  }

  // 2D capsule-ish walk test: the segment (or point, when both ends match)
  // against every box inflated by the body radius, filtered to boxes that
  // actually overlap the walking height band. Low pallets block movement
  // here even though eye-level sight passes straight over them.
  public walkBlocked(x0: number, z0: number, x1: number, z1: number): boolean {
    for (const b of this.boxes) {
      if (b.minY > WALK_BAND_HI || b.maxY < WALK_BAND_LO) continue;

      // segment into the box's local frame
      const px = b.cos * (x0 - b.cx) - b.sin * (z0 - b.cz);
      const pz = b.sin * (x0 - b.cx) + b.cos * (z0 - b.cz);
      const qx = b.cos * (x1 - b.cx) - b.sin * (z1 - b.cz);
      const qz = b.sin * (x1 - b.cx) + b.cos * (z1 - b.cz);
      const dx = qx - px;
      const dz = qz - pz;
      const limX = b.hw + BODY_RADIUS;
      const limZ = b.hd + BODY_RADIUS;

      let t0 = 0;
      let t1 = 1;
      if (Math.abs(dx) < 1e-8) {
        if (Math.abs(px) >= limX) continue;
      } else {
        let ta = (-limX - px) / dx;
        let tb = (limX - px) / dx;
        if (ta > tb) {
          const s = ta;
          ta = tb;
          tb = s;
        }
        t0 = Math.max(t0, ta);
        t1 = Math.min(t1, tb);
        if (t0 > t1) continue;
      }
      if (Math.abs(dz) < 1e-8) {
        if (Math.abs(pz) >= limZ) continue;
      } else {
        let ta = (-limZ - pz) / dz;
        let tb = (limZ - pz) / dz;
        if (ta > tb) {
          const s = ta;
          ta = tb;
          tb = s;
        }
        t0 = Math.max(t0, ta);
        t1 = Math.min(t1, tb);
        if (t0 > t1) continue;
      }
      return true;
    }
    return false;
  }

  // True 3D occlusion between two points (eye-to-eye sight, muzzle-to-chest
  // fire lanes). No inflation: you can see exactly what physically clears.
  public losBlocked(from: Vector3, to: Vector3): boolean {
    return this.segmentHit(from.x, from.y, from.z, to.x - from.x, to.y - from.y, to.z - from.z, 1, undefined, true) !== Infinity;
  }

  // Where does a bullet stop? Nearest OBB entry or the ground plane within
  // maxDist; fills outPoint/outNormal if provided and returns the distance, or Infinity.
  public rayHitWorld(origin: Vector3, dir: Vector3, maxDist: number, outPoint?: Vector3, outNormal?: Vector3): number {
    let best = this.segmentHit(origin.x, origin.y, origin.z, dir.x * maxDist, dir.y * maxDist, dir.z * maxDist, 1, outNormal);
    if (best !== Infinity) best *= maxDist;

    // ground plane
    if (dir.y < -1e-6) {
      const tFloor = -origin.y / dir.y;
      if (tFloor < best && tFloor <= maxDist) {
        best = tFloor;
        outNormal?.set(0, 1, 0);
      }
    }
    if (best === Infinity) return Infinity;
    outPoint?.set(origin.x + dir.x * best, origin.y + dir.y * best, origin.z + dir.z * best);
    return best;
  }

  private nextQueryId(): number {
    if (this.queryId >= 0x7fffffff) {
      this.boxStamp.fill(0);
      this.queryId = 0;
    }
    return ++this.queryId;
  }

  private testBox(
    bi: number,
    px: number,
    py: number,
    pz: number,
    dx: number,
    dy: number,
    dz: number,
    tMax: number,
    best: number,
    outNormal?: Vector3
  ): number {
    const b = this.boxes[bi];
    // into the box's local frame (y is shared — boxes only yaw)
    const ox = b.cos * (px - b.cx) - b.sin * (pz - b.cz);
    const oz = b.sin * (px - b.cx) + b.cos * (pz - b.cz);

    // Segment starts inside or on the boundary of the OBB: immediate hit at t = 0
    if (ox >= -b.hw && ox <= b.hw && oz >= -b.hd && oz <= b.hd && py >= b.minY && py <= b.maxY) {
      if (outNormal && 0 < best) {
        const distX = b.hw - Math.abs(ox);
        const distY = Math.min(py - b.minY, b.maxY - py);
        const distZ = b.hd - Math.abs(oz);
        if (distX <= distY && distX <= distZ) {
          const sign = ox >= 0 ? 1 : -1;
          outNormal.set(b.cos * sign, 0, -b.sin * sign);
        } else if (distY <= distZ) {
          outNormal.set(0, py - b.minY < b.maxY - py ? -1 : 1, 0);
        } else {
          const sign = oz >= 0 ? 1 : -1;
          outNormal.set(b.sin * sign, 0, b.cos * sign);
        }
      }
      return 0;
    }

    const ldx = b.cos * dx - b.sin * dz;
    const ldz = b.sin * dx + b.cos * dz;

    let t0 = 0;
    let t1 = tMax;
    let axis = -1; // which slab produced the entry (0 x, 1 y, 2 z)
    let sign = 0;

    // x slab
    if (Math.abs(ldx) < 1e-9) {
      if (Math.abs(ox) >= b.hw) return Infinity;
    } else {
      let ta = (-b.hw - ox) / ldx;
      let tb = (b.hw - ox) / ldx;
      const s = ldx > 0 ? -1 : 1;
      if (ta > tb) {
        const w = ta;
        ta = tb;
        tb = w;
      }
      if (ta > t0) {
        t0 = ta;
        axis = 0;
        sign = s;
      }
      t1 = Math.min(t1, tb);
      if (t0 > t1) return Infinity;
    }
    // y slab
    const cy = (b.minY + b.maxY) / 2;
    const hy = (b.maxY - b.minY) / 2;
    if (Math.abs(dy) < 1e-9) {
      if (Math.abs(py - cy) >= hy) return Infinity;
    } else {
      let ta = (cy - hy - py) / dy;
      let tb = (cy + hy - py) / dy;
      const s = dy > 0 ? -1 : 1;
      if (ta > tb) {
        const w = ta;
        ta = tb;
        tb = w;
      }
      if (ta > t0) {
        t0 = ta;
        axis = 1;
        sign = s;
      }
      t1 = Math.min(t1, tb);
      if (t0 > t1) return Infinity;
    }
    // z slab
    if (Math.abs(ldz) < 1e-9) {
      if (Math.abs(oz) >= b.hd) return Infinity;
    } else {
      let ta = (-b.hd - oz) / ldz;
      let tb = (b.hd - oz) / ldz;
      const s = ldz > 0 ? -1 : 1;
      if (ta > tb) {
        const w = ta;
        ta = tb;
        tb = w;
      }
      if (ta > t0) {
        t0 = ta;
        axis = 2;
        sign = s;
      }
      t1 = Math.min(t1, tb);
      if (t0 > t1) return Infinity;
    }

    if (t0 < best && t0 > 0 && axis >= 0) {
      if (outNormal) {
        // local face normal back out to world
        if (axis === 0) outNormal.set(b.cos * sign, 0, -b.sin * sign);
        else if (axis === 1) outNormal.set(0, sign, 0);
        else outNormal.set(b.sin * sign, 0, b.cos * sign);
      }
      return t0;
    }
    return Infinity;
  }

  // Core 3D slab test: segment (p, d) over t in [0, tMax] against OBBs in
  // traversed 8x8 spatial cells (plus any box extending outside [-16..16]).
  // Returns the smallest entry t (or Infinity) and, if a normal receiver is
  // given, the world-space normal of the face that was entered.
  private segmentHit(
    px: number,
    py: number,
    pz: number,
    dx: number,
    dy: number,
    dz: number,
    tMax: number,
    outNormal?: Vector3,
    anyHit = false
  ): number {
    if (this.boxes.length === 0) return Infinity;
    const qid = this.nextQueryId();
    let best = Infinity;

    for (let k = 0; k < this.outerBoxes.length; k++) {
      const bi = this.outerBoxes[k];
      if (this.boxStamp[bi] === qid) continue;
      this.boxStamp[bi] = qid;
      const hit = this.testBox(bi, px, py, pz, dx, dy, dz, tMax, best, outNormal);
      if (hit < best) {
        best = hit;
        if (anyHit || best === 0) return best;
      }
    }

    // Clip segment to the 8x8 spatial grid bounds [-16, 16]^2
    let tEnter = 0;
    let tExit = tMax;
    if (Math.abs(dx) < 1e-12) {
      if (px < CELL_MIN || px > CELL_MAX) return best;
    } else {
      let ta = (CELL_MIN - px) / dx;
      let tb = (CELL_MAX - px) / dx;
      if (ta > tb) {
        const tmp = ta;
        ta = tb;
        tb = tmp;
      }
      if (ta > tEnter) tEnter = ta;
      if (tb < tExit) tExit = tb;
      if (tEnter > tExit) return best;
    }
    if (Math.abs(dz) < 1e-12) {
      if (pz < CELL_MIN || pz > CELL_MAX) return best;
    } else {
      let ta = (CELL_MIN - pz) / dz;
      let tb = (CELL_MAX - pz) / dz;
      if (ta > tb) {
        const tmp = ta;
        ta = tb;
        tb = tmp;
      }
      if (ta > tEnter) tEnter = ta;
      if (tb < tExit) tExit = tb;
      if (tEnter > tExit) return best;
    }

    const sx = px + dx * tEnter;
    const sz = pz + dz * tEnter;
    let ix = Math.max(0, Math.min(CELL_GRID - 1, Math.floor((sx - CELL_MIN) / CELL_SIZE)));
    let iz = Math.max(0, Math.min(CELL_GRID - 1, Math.floor((sz - CELL_MIN) / CELL_SIZE)));

    let stepX = 0;
    let tDeltaX = Infinity;
    let tMaxX = Infinity;
    if (dx > 1e-12) {
      stepX = 1;
      tDeltaX = CELL_SIZE / dx;
      tMaxX = (CELL_MIN + (ix + 1) * CELL_SIZE - px) / dx;
    } else if (dx < -1e-12) {
      stepX = -1;
      tDeltaX = -CELL_SIZE / dx;
      tMaxX = (CELL_MIN + ix * CELL_SIZE - px) / dx;
    }

    let stepZ = 0;
    let tDeltaZ = Infinity;
    let tMaxZ = Infinity;
    if (dz > 1e-12) {
      stepZ = 1;
      tDeltaZ = CELL_SIZE / dz;
      tMaxZ = (CELL_MIN + (iz + 1) * CELL_SIZE - pz) / dz;
    } else if (dz < -1e-12) {
      stepZ = -1;
      tDeltaZ = -CELL_SIZE / dz;
      tMaxZ = (CELL_MIN + iz * CELL_SIZE - pz) / dz;
    }

    let tCur = tEnter;
    while (tCur <= tExit && tCur <= best) {
      const cell = this.grid[ix * CELL_GRID + iz];
      for (let k = 0; k < cell.length; k++) {
        const bi = cell[k];
        if (this.boxStamp[bi] === qid) continue;
        this.boxStamp[bi] = qid;
        const hit = this.testBox(bi, px, py, pz, dx, dy, dz, tMax, best, outNormal);
        if (hit < best) {
          best = hit;
          if (anyHit || best === 0) return best;
        }
      }
      if (tMaxX < tMaxZ) {
        tCur = tMaxX;
        tMaxX += tDeltaX;
        ix += stepX;
        if (ix < 0 || ix >= CELL_GRID) break;
      } else {
        tCur = tMaxZ;
        tMaxZ += tDeltaZ;
        iz += stepZ;
        if (iz < 0 || iz >= CELL_GRID) break;
      }
    }
    return best;
  }

  // Snap a world position to the closest walkable node (expanding ring
  // search handles positions hugging an obstacle's inflated skin)
  public nearestNode(x: number, z: number): number {
    const ix = Math.round((x + GRID_HALF) / GRID_STEP);
    const iz = Math.round((z + GRID_HALF) / GRID_STEP);
    for (let r = 0; r <= 3; r++) {
      let bestNode = -1;
      let bestD2 = Infinity;
      for (let dx = -r; dx <= r; dx++) {
        for (let dz = -r; dz <= r; dz++) {
          if (Math.max(Math.abs(dx), Math.abs(dz)) !== r) continue; // ring only
          const nx = ix + dx;
          const nz = iz + dz;
          if (nx < 0 || nz < 0 || nx >= this.side || nz >= this.side) continue;
          const n = nx * this.side + nz;
          if (!this.walkable[n]) continue;
          const ddx = this.xs[n] - x;
          const ddz = this.zs[n] - z;
          const d2 = ddx * ddx + ddz * ddz;
          if (d2 < bestD2) {
            bestD2 = d2;
            bestNode = n;
          }
        }
      }
      if (bestNode >= 0) return bestNode;
    }
    return -1;
  }

  // A random walkable node within the radius — patrol destinations
  public randomNodeNear(x: number, z: number, radius: number): number {
    for (let attempt = 0; attempt < 14; attempt++) {
      const n = (Math.random() * this.count) | 0;
      if (!this.walkable[n]) continue;
      const dx = this.xs[n] - x;
      const dz = this.zs[n] - z;
      if (dx * dx + dz * dz < radius * radius) return n;
    }
    return -1;
  }

  private siftUp(n: number, idx: number): void {
    const fn = this.f[n];
    let i = idx;
    while (i > 0) {
      const p = (i - 1) >> 1;
      const pn = this.heap[p];
      if (this.f[pn] <= fn) break;
      this.heap[i] = pn;
      this.heapPos[pn] = i;
      i = p;
    }
    this.heap[i] = n;
    this.heapPos[n] = i;
  }

  private heapPop(): number {
    const top = this.heap[0];
    this.heapPos[top] = -1;
    const size = --this.heapSize;
    if (size > 0) {
      const n = this.heap[size];
      const fn = this.f[n];
      let i = 0;
      const half = size >> 1;
      while (i < half) {
        let c = (i << 1) + 1;
        const r = c + 1;
        if (r < size && this.f[this.heap[r]] < this.f[this.heap[c]]) c = r;
        const cn = this.heap[c];
        if (fn <= this.f[cn]) break;
        this.heap[i] = cn;
        this.heapPos[cn] = i;
        i = c;
      }
      this.heap[i] = n;
      this.heapPos[n] = i;
    }
    return top;
  }

  // A* over the grid (euclidean heuristic) using a reusable typed binary
  // min-heap and precomputed edge costs. Fills `out` start..goal.
  public findPath(start: number, goal: number, out: number[]): boolean {
    out.length = 0;
    if (start < 0 || goal < 0 || !this.walkable[start] || !this.walkable[goal]) return false;
    if (start === goal) {
      out.push(start);
      return true;
    }

    this.g.fill(Infinity);
    this.state.fill(0);
    this.heapSize = 0;
    const gx = this.xs[goal];
    const gz = this.zs[goal];
    this.g[start] = 0;
    this.f[start] = Math.hypot(this.xs[start] - gx, this.zs[start] - gz);
    this.state[start] = 1;
    this.siftUp(start, this.heapSize++);

    while (this.heapSize > 0) {
      const current = this.heapPop();
      this.state[current] = 2;

      if (current === goal) {
        let n = goal;
        while (n !== start) {
          out.push(n);
          n = this.came[n];
        }
        out.push(start);
        out.reverse();
        return true;
      }

      const nbrs = this.edges[current];
      const costs = this.edgeCosts[current];
      for (let k = 0; k < nbrs.length; k++) {
        const n = nbrs[k];
        if (this.state[n] === 2) continue;
        const cost = this.g[current] + costs[k];
        if (cost < this.g[n]) {
          this.g[n] = cost;
          this.came[n] = current;
          const hx = this.xs[n] - gx;
          const hz = this.zs[n] - gz;
          this.f[n] = cost + Math.sqrt(hx * hx + hz * hz);
          if (this.state[n] !== 1) {
            this.state[n] = 1;
            this.siftUp(n, this.heapSize++);
          } else {
            this.siftUp(n, this.heapPos[n]);
          }
        }
      }
    }
    return false;
  }
}

// Single shared instance — the same pattern as PlayerController's static
// obstacle registry it is built from
export const BotNav = new BotNavSystem();
