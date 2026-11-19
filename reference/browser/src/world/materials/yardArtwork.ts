import { createPrng, paintStreaks } from "../../rendering/materials/canvas";

// Procedural artwork for the Ship Box yard: the graffiti wall atlas and the
// two hoarding posters. Pure canvas painters: no scene, no Babylon, so
// WorldMaterials keeps only the material wiring around them.

export function paintGraffitiAtlas(ctx: CanvasRenderingContext2D): void {
  // The artwork is laid out in wall proportions (8.45m × 2.55m per
  // panel). The atlas compresses x to 512px, so letters retain their
  // intended proportions when those UVs cover the real wall geometry.
  const panelW = 800;
  const panelH = 256;
  const random = createPrng(0x51f15e);

  type Piece = {
    word: string;
    verse: number;
    style: "angular" | "bubble" | "marker";
    x: number;
    y: number;
    angle: number;
    top: string;
    bottom: string;
    edge: string;
  };
  // Excerpts from the Dhammapada, F. Max Muller's public-domain translation.
  // https://www.gutenberg.org/files/2017/2017-h/2017-h.htm
  const pieces: Piece[] = [
    {
      word: "HATRED CEASES BY LOVE,\nTHIS IS AN OLD RULE.",
      verse: 5,
      style: "angular",
      x: 365,
      y: 142,
      angle: -0.055,
      top: "#e9ad61",
      bottom: "#b85b38",
      edge: "#eee0bf",
    },
    {
      word: "A TAMED MIND\nBRINGS HAPPINESS.",
      verse: 35,
      style: "marker",
      x: 445,
      y: 151,
      angle: -0.12,
      top: "#393b37",
      bottom: "#393b37",
      edge: "#a5a197",
    },
    {
      word: "THOUGHTS WELL GUARDED\nBRING HAPPINESS.",
      verse: 36,
      style: "bubble",
      x: 402,
      y: 135,
      angle: 0.025,
      top: "#9ebcb1",
      bottom: "#3d7b7e",
      edge: "#ded7c2",
    },
    {
      word: "AS A SOLID ROCK IS NOT\nSHAKEN BY THE WIND,\nWISE PEOPLE FALTER NOT\nAMIDST BLAME AND PRAISE.",
      verse: 81,
      style: "angular",
      x: 354,
      y: 143,
      angle: -0.045,
      top: "#d2d0c3",
      bottom: "#899192",
      edge: "#343e46",
    },
    {
      word: "HAPPINESS IS THE\nOUTCOME OF GOOD.",
      verse: 118,
      style: "bubble",
      x: 412,
      y: 137,
      angle: -0.04,
      top: "#a8bacc",
      bottom: "#4c719d",
      edge: "#e2ddc7",
    },
    {
      word: "DO NOT SPEAK HARSHLY\nTO ANYBODY.",
      verse: 133,
      style: "marker",
      x: 280,
      y: 146,
      angle: 0.035,
      top: "#d8d1b9",
      bottom: "#d8d1b9",
      edge: "#696e65",
    },
    {
      word: "GOOD PEOPLE\nFASHION THEMSELVES.",
      verse: 145,
      style: "angular",
      x: 408,
      y: 134,
      angle: -0.02,
      top: "#789bb0",
      bottom: "#3d617e",
      edge: "#c8caba",
    },
    {
      word: "LET EACH MAN DIRECT\nHIMSELF FIRST TO\nWHAT IS PROPER.",
      verse: 158,
      style: "marker",
      x: 510,
      y: 154,
      angle: -0.11,
      top: "#323631",
      bottom: "#323631",
      edge: "#aaa69b",
    },
    {
      word: "SELF IS THE LORD\nOF SELF, WHO ELSE\nCOULD BE THE LORD?",
      verse: 160,
      style: "angular",
      x: 319,
      y: 141,
      angle: 0.05,
      top: "#b8a5b7",
      bottom: "#79627e",
      edge: "#dbd1bc",
    },
    {
      word: "PURITY AND IMPURITY\nBELONG TO ONESELF,\nNO ONE CAN\nPURIFY ANOTHER.",
      verse: 165,
      style: "bubble",
      x: 426,
      y: 145,
      angle: -0.055,
      top: "#bbb996",
      bottom: "#7f865e",
      edge: "#d9d4bb",
    },
    {
      word: "HIS OWN MISDEEDS\nAND NEGLIGENCES\nSHOULD A SAGE\nTAKE NOTICE OF.",
      verse: 50,
      style: "marker",
      x: 318,
      y: 155,
      angle: -0.07,
      top: "#353a36",
      bottom: "#353a36",
      edge: "#96998c",
    },
    {
      word: "LET US LIVE HAPPILY THEN,\nNOT HATING THOSE\nWHO HATE US!",
      verse: 197,
      style: "angular",
      x: 443,
      y: 136,
      angle: 0.035,
      top: "#d7d0b9",
      bottom: "#a39a83",
      edge: "#405c64",
    },
    {
      word: "VICTORY BREEDS HATRED,\nFOR THE CONQUERED\nIS UNHAPPY.",
      verse: 201,
      style: "marker",
      x: 388,
      y: 135,
      angle: -0.08,
      top: "#5b6258",
      bottom: "#5b6258",
      edge: "#b5b2a5",
    },
    {
      word: "HEALTH IS THE\nGREATEST OF GIFTS.",
      verse: 204,
      style: "bubble",
      x: 373,
      y: 142,
      angle: -0.02,
      top: "#c7a293",
      bottom: "#9b6056",
      edge: "#d6d0b8",
    },
    {
      word: "LET A MAN OVERCOME\nANGER BY LOVE.",
      verse: 223,
      style: "angular",
      x: 429,
      y: 131,
      angle: 0.045,
      top: "#ceb875",
      bottom: "#978349",
      edge: "#36474b",
    },
    {
      word: "YOU YOURSELF MUST\nMAKE AN EFFORT.",
      verse: 276,
      style: "marker",
      x: 507,
      y: 150,
      angle: 0.02,
      top: "#d5d0bc",
      bottom: "#d5d0bc",
      edge: "#4c554f",
    },
  ];

  const fontFor = (style: Piece["style"], px: number): string => {
    if (style === "marker") return `italic 700 ${px}px Georgia, "Courier New", monospace`;
    if (style === "bubble") return `900 ${px}px "Trebuchet MS", "Arial Rounded MT Bold", Arial, sans-serif`;
    return `900 ${px}px Impact, "Arial Black", "Arial Narrow", sans-serif`;
  };

  const drawQuote = (piece: Piece): void => {
    const lines = piece.word.split("\n");
    const marker = piece.style === "marker";
    const round = piece.style === "bubble";
    const refPx = 48;
    ctx.font = fontFor(piece.style, refPx);
    const widest = Math.max(...lines.map((line) => ctx.measureText(line).width), 1);
    const fontSize = Math.min(marker ? 36 : 46, (610 / widest) * refPx, 136 / (lines.length * 1.18));
    const spacing = fontSize * 1.18;
    const halfHeight = ((lines.length - 1) * spacing) / 2;

    ctx.save();
    ctx.translate(panelW / 2, 114);
    ctx.rotate(piece.angle);
    ctx.font = fontFor(piece.style, fontSize);
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.lineJoin = "round";
    ctx.lineCap = "round";

    lines.forEach((line, i) => {
      const ly = i * spacing - halfHeight;
      ctx.save();
      ctx.translate(0, ly);
      ctx.transform(1, 0, marker ? -0.22 : -0.08, 1, 0, 0);
      if (marker) {
        ctx.globalAlpha = 0.6;
        ctx.strokeStyle = piece.edge;
        ctx.lineWidth = Math.max(3, fontSize * 0.16);
        ctx.strokeText(line, 0, 0);
        ctx.globalAlpha = 0.88;
        ctx.fillStyle = piece.top;
        ctx.fillText(line, 0, 0);
      } else {
        const fill = ctx.createLinearGradient(0, -fontSize * 0.55, 8, fontSize * 0.55);
        fill.addColorStop(0, piece.top);
        fill.addColorStop(0.6, piece.bottom);
        fill.addColorStop(1, piece.top);

        // Drop shadow
        ctx.save();
        ctx.translate(4, 6);
        ctx.globalAlpha = 0.48;
        ctx.strokeStyle = "#303632";
        ctx.lineWidth = fontSize * (round ? 0.34 : 0.22);
        ctx.strokeText(line, 0, 0);
        ctx.fillStyle = "#303632";
        ctx.fillText(line, 0, 0);
        ctx.restore();

        // Outer edge halo + dark keyline + gradient fill
        ctx.globalAlpha = 0.95;
        ctx.strokeStyle = piece.edge;
        ctx.lineWidth = fontSize * (round ? 0.34 : 0.22);
        ctx.strokeText(line, 0, 0);
        ctx.strokeStyle = "#252d2b";
        ctx.lineWidth = fontSize * (round ? 0.16 : 0.11);
        ctx.strokeText(line, 0, 0);
        ctx.fillStyle = fill;
        ctx.fillText(line, 0, 0);

        // Broken highlight glint on angular pieces
        if (!round) {
          const halfW = Math.min(280, ctx.measureText(line).width * 0.42);
          ctx.globalAlpha = 0.28;
          ctx.strokeStyle = "#f0e6ce";
          ctx.lineWidth = 1.6;
          ctx.beginPath();
          ctx.moveTo(-halfW, -fontSize * 0.26);
          ctx.lineTo(-halfW * 0.25, -fontSize * 0.29);
          ctx.moveTo(halfW * 0.12, -fontSize * 0.31);
          ctx.lineTo(halfW * 0.8, -fontSize * 0.28);
          ctx.stroke();
        }
      }
      ctx.restore();
    });

    ctx.globalAlpha = 0.9;
    ctx.fillStyle = "#293b32";
    ctx.font = "bold 15px Arial, sans-serif";
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(`BUDDHA / DHAMMAPADA ${piece.verse}`, 0, halfHeight + fontSize * 0.65 + 22);
    ctx.restore();
  };

  for (let row = 0; row < 4; row++) {
    ctx.save();
    ctx.translate(0, row * panelH);
    ctx.beginPath();
    ctx.rect(0, 0, 2048, panelH);
    ctx.clip();
    ctx.fillStyle = ["#92958b", "#8e948a", "#999b90", "#91978c"][row];
    ctx.fillRect(0, 0, 2048, panelH);
    for (let i = 0; i < 420; i++) {
      ctx.globalAlpha = 0.035 + random() * 0.06;
      ctx.fillStyle = i % 3 ? "#616d64" : "#c5c5b3";
      ctx.beginPath();
      ctx.ellipse(random() * 2048, random() * panelH, 3 + random() * 37, 2 + random() * 14, random(), 0, Math.PI * 2);
      ctx.fill();
    }
    // Damp footing and runoff anchor the paint to the wall surface.
    ctx.globalAlpha = 1;
    const damp = ctx.createLinearGradient(0, 181, 0, panelH);
    damp.addColorStop(0, "rgba(54,70,55,0)");
    damp.addColorStop(1, "rgba(54,70,55,0.3)");
    ctx.fillStyle = damp;
    ctx.fillRect(0, 0, 2048, panelH);
    paintStreaks(ctx, panelH, {
      color: "#35433a",
      count: 25,
      minW: 1,
      maxW: 8,
      minLen: 65,
      maxLen: 245,
      minAlpha: 0.025,
      maxAlpha: 0.06,
      minX: 40,
      maxX: 2008,
      minY: 8,
      maxY: 8,
      rng: random,
    });

    for (let col = 0; col < 4; col++) {
      const piece = pieces[row * 4 + col];
      ctx.save();
      ctx.translate(col * 512, 0);
      ctx.scale(512 / panelW, 1);
      // Older buff paint sits behind the quotation; no unrelated tags
      // or signatures remain on the wall.
      ctx.globalAlpha = piece.style === "marker" ? 0.3 : 0.17;
      ctx.fillStyle = col % 2 ? "#b2b1a3" : "#757e72";
      ctx.beginPath();
      const bx = 125 + ((row * 97 + col * 43) % 180);
      ctx.moveTo(bx, 65);
      ctx.lineTo(bx + 303, 61 + col * 4);
      ctx.lineTo(bx + 294, 184);
      ctx.lineTo(bx - 7, 179);
      ctx.closePath();
      ctx.fill();
      ctx.globalAlpha = 0.12;
      ctx.strokeStyle = "#ddd7c3";
      ctx.lineWidth = 9;
      ctx.beginPath();
      ctx.moveTo(bx + 8, 73);
      ctx.lineTo(bx + 288, 68);
      ctx.stroke();

      if (piece.style !== "marker") {
        for (let i = 0; i < 95; i++) {
          const a = random() * Math.PI * 2;
          const r = Math.sqrt(random());
          ctx.globalAlpha = 0.04 + random() * 0.05;
          ctx.fillStyle = i % 2 ? piece.top : piece.bottom;
          ctx.fillRect(piece.x + Math.cos(a) * r * 230, piece.y + Math.sin(a) * r * 76, 1 + random() * 2, 1);
        }
      }
      drawQuote(piece);

      // Gravity-led paint runs & splatter accents below the lettering.
      if (piece.style !== "marker") {
        for (let i = 0; i < 4; i++) {
          const dx = piece.x - 135 + i * 90 + random() * 20;
          const len = 9 + random() * 20;
          ctx.globalAlpha = 0.62;
          ctx.strokeStyle = i % 2 ? piece.bottom : "#303830";
          ctx.lineCap = "round";
          ctx.lineWidth = 1.2 + random();
          ctx.beginPath();
          ctx.moveTo(dx, 200);
          ctx.lineTo(dx + 0.8, Math.min(237, 200 + len));
          ctx.stroke();
        }
      }
      for (let i = 0; i < 130; i++) {
        ctx.globalAlpha = 0.08 + random() * 0.16;
        ctx.fillStyle = i % 4 ? "#9ca193" : "#525f52";
        ctx.fillRect(62 + random() * 698, 27 + random() * 210, 1 + random() * 3, 0.7 + random() * 1.4);
      }
      if ((row + col) % 3 === 0) {
        ctx.globalAlpha = 0.23;
        ctx.strokeStyle = "#4d5b4f";
        ctx.lineWidth = 0.65;
        ctx.beginPath();
        ctx.moveTo(727, 10);
        ctx.lineTo(715, 64);
        ctx.lineTo(725, 88);
        ctx.lineTo(701, 150);
        ctx.lineTo(707, 203);
        ctx.stroke();
      }
      ctx.restore();
    }
    // Keep the atlas sampling margins free of lettering; box ends use
    // the first 24px, and filtered long faces omit the top/bottom 8px.
    ctx.globalAlpha = 0.42;
    ctx.fillStyle = "#969c8f";
    ctx.fillRect(0, 0, 32, panelH);
    ctx.globalAlpha = 1;
    ctx.restore();
  }
}

// "MISPLACED FREIGHT" — a container-line advert: bold diagonal stripe, a
// stacked-container silhouette under a crane, heavy grotesque lettering
export function paintFreightPoster(ctx: CanvasRenderingContext2D, w: number, h: number): void {
  ctx.fillStyle = "#d8cfb8";
  ctx.fillRect(0, 0, w, h);
  // paper grain
  for (let i = 0; i < 2600; i++) {
    ctx.fillStyle = i % 2 ? "rgba(90,80,60,0.08)" : "rgba(255,250,235,0.1)";
    ctx.fillRect(Math.random() * w, Math.random() * h, 2, 2);
  }
  // diagonal brand stripe
  ctx.save();
  ctx.translate(w / 2, h / 2);
  ctx.rotate(-0.42);
  ctx.fillStyle = "#b8352b";
  ctx.fillRect(-w, -h * 0.07, w * 2, h * 0.14);
  ctx.fillStyle = "#1f2a35";
  ctx.fillRect(-w, h * 0.075, w * 2, h * 0.025);
  ctx.restore();

  // container stack silhouette
  const stackX = w * 0.12;
  const stackY = h * 0.52;
  const cw = w * 0.28;
  const ch = h * 0.075;
  const colors = ["#2c5d7a", "#8a4a2c", "#3f6b3a", "#5d5d63", "#a8722e"];
  for (let row = 0; row < 4; row++) {
    for (let col = 0; col < 2; col++) {
      const x = stackX + col * (cw + 6) + (row % 2) * 18;
      const y = stackY + row * (ch + 4);
      ctx.fillStyle = colors[(row * 2 + col) % colors.length];
      ctx.fillRect(x, y, cw, ch);
      ctx.fillStyle = "rgba(0,0,0,0.25)";
      for (let rib = 0; rib < 9; rib++) ctx.fillRect(x + 8 + rib * (cw / 9), y + 4, 3, ch - 8);
    }
  }
  // gantry crane
  ctx.strokeStyle = "#1f2a35";
  ctx.lineWidth = 9;
  ctx.beginPath();
  ctx.moveTo(w * 0.7, h * 0.86);
  ctx.lineTo(w * 0.7, h * 0.42);
  ctx.lineTo(w * 0.2, h * 0.42);
  ctx.moveTo(w * 0.86, h * 0.86);
  ctx.lineTo(w * 0.86, h * 0.42);
  ctx.lineTo(w * 1.02, h * 0.42);
  ctx.stroke();
  ctx.lineWidth = 4;
  ctx.beginPath();
  ctx.moveTo(w * 0.7, h * 0.5);
  ctx.lineTo(w * 0.86, h * 0.78);
  ctx.moveTo(w * 0.86, h * 0.5);
  ctx.lineTo(w * 0.7, h * 0.78);
  ctx.stroke();
  ctx.fillStyle = "#1f2a35";
  ctx.fillRect(w * 0.46, h * 0.44, w * 0.06, h * 0.05); // trolley
  ctx.fillRect(w * 0.485, h * 0.49, w * 0.01, h * 0.09); // hoist cable
  ctx.fillStyle = "#b8352b";
  ctx.fillRect(w * 0.42, h * 0.58, w * 0.14, h * 0.05); // lifted box

  // lettering
  ctx.fillStyle = "#1f2a35";
  ctx.textAlign = "left";
  ctx.textBaseline = "alphabetic";
  ctx.font = `900 ${Math.round(h * 0.11)}px "Arial Black", Impact, sans-serif`;
  ctx.fillText("MISPLACED", w * 0.06, h * 0.17, w * 0.88);
  ctx.fillText("FREIGHT", w * 0.06, h * 0.28, w * 0.88);
  ctx.fillStyle = "#b8352b";
  ctx.font = `700 ${Math.round(h * 0.034)}px Arial, sans-serif`;
  ctx.fillText("IT'S NOT LOST. IT'S ON TOUR.", w * 0.06, h * 0.33, w * 0.88);
  ctx.fillStyle = "#1f2a35";
  ctx.font = `700 ${Math.round(h * 0.03)}px Arial, sans-serif`;
  ctx.fillText("TRACKING: VIBES ONLY.", w * 0.06, h * 0.915, w * 0.88);
  ctx.fillText("DELIVERY DATE: A SURPRISE.", w * 0.06, h * 0.95, w * 0.88);
}

// "PANIC FM 98.3" — a dockside radio poster: halftone sunburst, a bold
// numeral, and a broadcast mast throwing rings
export function paintRadioPoster(ctx: CanvasRenderingContext2D, w: number, h: number): void {
  const sky = ctx.createLinearGradient(0, 0, 0, h);
  sky.addColorStop(0, "#1b2233");
  sky.addColorStop(0.65, "#3a4f6b");
  sky.addColorStop(1, "#c9a45c");
  ctx.fillStyle = sky;
  ctx.fillRect(0, 0, w, h);
  // sunburst rays
  ctx.save();
  ctx.translate(w / 2, h * 0.72);
  for (let i = 0; i < 24; i++) {
    ctx.rotate(Math.PI / 12);
    ctx.fillStyle = i % 2 ? "rgba(233,190,92,0.22)" : "rgba(233,190,92,0.06)";
    ctx.beginPath();
    ctx.moveTo(0, 0);
    ctx.lineTo(-h * 0.09, -h * 1.2);
    ctx.lineTo(h * 0.09, -h * 1.2);
    ctx.closePath();
    ctx.fill();
  }
  ctx.restore();
  // halftone field low on the sheet
  for (let y = h * 0.5; y < h; y += 14) {
    for (let x = 0; x < w; x += 14) {
      const t = (y - h * 0.5) / (h * 0.5);
      ctx.fillStyle = "rgba(20,24,34,0.55)";
      ctx.beginPath();
      ctx.arc(x, y, 1 + t * 5, 0, Math.PI * 2);
      ctx.fill();
    }
  }
  // mast + rings
  ctx.strokeStyle = "#f2e6c8";
  ctx.lineWidth = 6;
  ctx.beginPath();
  ctx.moveTo(w * 0.5, h * 0.74);
  ctx.lineTo(w * 0.5, h * 0.36);
  ctx.stroke();
  ctx.lineWidth = 3;
  for (let r = 1; r <= 4; r++) {
    ctx.globalAlpha = 1 - r * 0.18;
    ctx.beginPath();
    ctx.arc(w * 0.5, h * 0.36, r * w * 0.075, Math.PI * 1.15, Math.PI * 1.85);
    ctx.stroke();
  }
  ctx.globalAlpha = 1;
  // numeral and name
  ctx.fillStyle = "#f2e6c8";
  ctx.textAlign = "center";
  ctx.font = `900 ${Math.round(h * 0.2)}px "Arial Black", Impact, sans-serif`;
  ctx.fillText("98.3", w * 0.5, h * 0.9, w * 0.88);
  ctx.font = `900 ${Math.round(h * 0.07)}px "Arial Black", Impact, sans-serif`;
  ctx.fillStyle = "#e9be5c";
  ctx.fillText("PANIC FM", w * 0.5, h * 0.2, w * 0.88);
  ctx.font = `700 ${Math.round(h * 0.026)}px Arial, sans-serif`;
  ctx.fillStyle = "#f2e6c8";
  ctx.fillText("THE VOICES ARE PROBABLY US.", w * 0.5, h * 0.25, w * 0.88);
  ctx.fillText("ALL ADS. OCCASIONAL MUSIC.", w * 0.5, h * 0.955, w * 0.88);
}

// Rain-yard weathering shared by both prints: desaturate toward the
// overcast palette, darken the edges, drip streaks, grime, paste seams
export function weatherPoster(ctx: CanvasRenderingContext2D, w: number, h: number): void {
  ctx.globalCompositeOperation = "saturation";
  ctx.fillStyle = "rgba(128, 128, 128, 0.4)";
  ctx.fillRect(0, 0, w, h);

  ctx.globalCompositeOperation = "multiply";
  const vig = ctx.createRadialGradient(w / 2, h / 2, h * 0.25, w / 2, h / 2, h * 0.72);
  vig.addColorStop(0, "#ffffff");
  vig.addColorStop(1, "#969c9a");
  ctx.fillStyle = vig;
  ctx.fillRect(0, 0, w, h);

  paintStreaks(ctx, h, {
    color: "#5c605c",
    count: 24,
    minW: 3,
    maxW: 14,
    minLen: h * 0.25,
    maxLen: h,
    minAlpha: 0.45,
    maxAlpha: 0.45,
    minX: 0,
    maxX: w,
    maxY: 0,
    fade: true,
  });
  for (let i = 0; i < 12; i++) {
    const bx = Math.random() * w;
    const by = Math.random() * h;
    const br = h * 0.03 + Math.random() * h * 0.11;
    const blot = ctx.createRadialGradient(bx, by, 0, bx, by, br);
    blot.addColorStop(0, "rgba(86, 84, 76, 0.22)");
    blot.addColorStop(1, "rgba(86, 84, 76, 0)");
    ctx.fillStyle = blot;
    ctx.fillRect(bx - br, by - br, br * 2, br * 2);
  }
  // paste-sheet seams — hoardings go up in panels
  ctx.fillStyle = "rgba(70, 72, 70, 0.28)";
  ctx.fillRect(0, h * 0.34, w, 2);
  ctx.fillRect(0, h * 0.67, w, 2);
  ctx.fillRect(w * 0.5, 0, 2, h);
  // torn corner
  ctx.fillStyle = "#4a4d4b";
  ctx.beginPath();
  ctx.moveTo(w, h);
  ctx.lineTo(w - w * 0.16, h);
  ctx.lineTo(w - w * 0.05, h - h * 0.06);
  ctx.lineTo(w, h - h * 0.11);
  ctx.closePath();
  ctx.fill();

  ctx.globalCompositeOperation = "screen";
  const wash = ctx.createLinearGradient(0, 0, 0, h * 0.3);
  wash.addColorStop(0, "rgba(140, 148, 150, 0.16)");
  wash.addColorStop(1, "rgba(140, 148, 150, 0)");
  ctx.fillStyle = wash;
  ctx.fillRect(0, 0, w, h * 0.3);
  ctx.globalCompositeOperation = "source-over";
}
