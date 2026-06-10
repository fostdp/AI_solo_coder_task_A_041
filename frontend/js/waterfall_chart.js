let nextTransferId = 0;

class WaterfallWorker {
    constructor() {
        this.workerCode = `
            let colorMap = null;
            let offscreen = null;
            let ctx = null;
            let width = 0;
            let height = 0;

            function createColorMap() {
                const colors = new Uint8Array(256 * 3);
                for (let i = 0; i < 256; i++) {
                    const t = i / 255;
                    let r, g, b;
                    if (t < 0.25) {
                        const t2 = t / 0.25;
                        r = 0;
                        g = Math.floor(t2 * 128);
                        b = Math.floor(64 + t2 * 191);
                    } else if (t < 0.5) {
                        const t2 = (t - 0.25) / 0.25;
                        r = Math.floor(t2 * 128);
                        g = Math.floor(128 + t2 * 127);
                        b = Math.floor(255 - t2 * 128);
                    } else if (t < 0.75) {
                        const t2 = (t - 0.5) / 0.25;
                        r = Math.floor(128 + t2 * 127);
                        g = Math.floor(255 - t2 * 128);
                        b = Math.floor(127 - t2 * 127);
                    } else {
                        const t2 = (t - 0.75) / 0.25;
                        r = 255;
                        g = Math.floor(127 - t2 * 127);
                        b = Math.floor(t2 * 64);
                    }
                    colors[i * 3] = r;
                    colors[i * 3 + 1] = g;
                    colors[i * 3 + 2] = b;
                }
                return colors;
            }

            self.onmessage = function(e) {
                const msg = e.data;

                if (msg.type === 'init') {
                    colorMap = createColorMap();
                    offscreen = new OffscreenCanvas(msg.width, msg.height);
                    ctx = offscreen.getContext('2d');
                    width = msg.width;
                    height = msg.height;
                    return;
                }

                if (msg.type === 'resize') {
                    width = msg.width;
                    height = msg.height;
                    if (offscreen) {
                        offscreen.width = width;
                        offscreen.height = height;
                    }
                    return;
                }

                if (msg.type === 'render') {
                    if (!ctx || !colorMap) return;

                    const { chunks, frequencyBins, maxLines, minVal, maxVal,
                            chartLeft, chartTop, chartWidth, chartHeight,
                            maxFreq } = msg;

                    ctx.fillStyle = '#0a0f1a';
                    ctx.fillRect(0, 0, width, height);

                    if (chunks.length === 0) {
                        ctx.fillStyle = '#555';
                        ctx.font = '14px sans-serif';
                        ctx.textAlign = 'center';
                        ctx.fillText('等待数据...', width / 2, height / 2);
                        const bitmap = offscreen.transferToImageBitmap();
                        self.postMessage({ type: 'frame', bitmap }, [bitmap]);
                        return;
                    }

                    const lineHeight = chartHeight / maxLines;
                    const valRange = (maxVal - minVal) || 1;

                    const imageData = ctx.createImageData(chartWidth, chartHeight);
                    const pixels = imageData.data;

                    let lineIdx = 0;
                    for (const chunk of chunks) {
                        for (let c = 0; c < chunk.length && lineIdx < maxLines; c++, lineIdx++) {
                            const line = chunk[c];
                            if (!line) continue;

                            const y = Math.floor(lineIdx * lineHeight);
                            const lineH = Math.max(1, Math.ceil(lineHeight));

                            for (let py = y; py < y + lineH && py < chartHeight; py++) {
                                for (let px = 0; px < chartWidth; px++) {
                                    const freqIdx = Math.floor(px / chartWidth * line.length);
                                    const val = line[freqIdx] || 0;
                                    const normalized = (val - minVal) / valRange;
                                    const colorIdx = Math.max(0, Math.min(255, Math.floor(normalized * 255)));

                                    const pidx = (py * chartWidth + px) * 4;
                                    pixels[pidx]     = colorMap[colorIdx * 3];
                                    pixels[pidx + 1] = colorMap[colorIdx * 3 + 1];
                                    pixels[pidx + 2] = colorMap[colorIdx * 3 + 2];
                                    pixels[pidx + 3] = 255;
                                }
                            }
                        }
                    }

                    ctx.putImageData(imageData, chartLeft, chartTop);

                    ctx.strokeStyle = '#444';
                    ctx.lineWidth = 0.5;
                    for (let i = 0; i <= 5; i++) {
                        const x = chartLeft + (i / 5) * chartWidth;
                        ctx.beginPath();
                        ctx.moveTo(x, chartTop);
                        ctx.lineTo(x, chartTop + chartHeight);
                        ctx.stroke();
                    }

                    ctx.fillStyle = '#888';
                    ctx.font = '12px sans-serif';
                    ctx.textAlign = 'center';
                    for (let i = 0; i <= 5; i++) {
                        const freq = (i / 5) * maxFreq;
                        const x = chartLeft + (i / 5) * chartWidth;
                        ctx.fillText((freq / 1000).toFixed(0) + ' kHz', x, chartTop + chartHeight + 20);
                    }

                    ctx.textAlign = 'right';
                    ctx.fillText('最新', chartLeft - 10, chartTop + 15);
                    ctx.fillText('-' + (maxLines / 10) + 's', chartLeft - 10, chartTop + chartHeight / 2);
                    ctx.fillText('-' + (maxLines / 5) + 's', chartLeft - 10, chartTop + chartHeight);

                    ctx.strokeStyle = '#333';
                    ctx.lineWidth = 2;
                    ctx.strokeRect(chartLeft, chartTop, chartWidth, chartHeight);

                    ctx.fillStyle = '#fff';
                    ctx.font = '12px sans-serif';
                    ctx.textAlign = 'center';
                    ctx.fillText('频率 (kHz)', width / 2, height - 10);

                    const cbX = chartLeft + chartWidth + 10;
                    const cbY = chartTop;
                    const cbW = 20;
                    const cbH = chartHeight;
                    const cbSteps = 64;
                    for (let i = 0; i < cbSteps; i++) {
                        const t = i / cbSteps;
                        const ci = Math.floor(t * 255);
                        ctx.fillStyle = 'rgb(' + colorMap[ci*3] + ',' + colorMap[ci*3+1] + ',' + colorMap[ci*3+2] + ')';
                        ctx.fillRect(cbX, cbY + cbH * (1 - t) - cbH / cbSteps, cbW, cbH / cbSteps + 1);
                    }
                    ctx.strokeStyle = '#666';
                    ctx.lineWidth = 1;
                    ctx.strokeRect(cbX, cbY, cbW, cbH);

                    ctx.fillStyle = '#888';
                    ctx.font = '11px sans-serif';
                    ctx.textAlign = 'left';
                    ctx.fillText(maxVal.toFixed(2), cbX + cbW + 5, cbY + 10);
                    ctx.fillText(((minVal + maxVal) / 2).toFixed(2), cbX + cbW + 5, cbY + cbH / 2);
                    ctx.fillText(minVal.toFixed(2), cbX + cbW + 5, cbY + cbH - 2);

                    const bitmap = offscreen.transferToImageBitmap();
                    self.postMessage({ type: 'frame', bitmap }, [bitmap]);
                }
            };
        `;

        const blob = new Blob([this.workerCode], { type: 'application/javascript' });
        this.url = URL.createObjectURL(blob);
        this.worker = new Worker(this.url);
        this.pendingCallbacks = {};
        this.worker.onmessage = (e) => {
            if (e.data.type === 'frame' && this.onFrame) {
                this.onFrame(e.data.bitmap);
            }
        };
    }

    init(width, height) {
        this.worker.postMessage({ type: 'init', width, height });
    }

    resize(width, height) {
        this.worker.postMessage({ type: 'resize', width, height });
    }

    render(data) {
        this.worker.postMessage(data, []);
    }

    destroy() {
        this.worker.terminate();
        URL.revokeObjectURL(this.url);
    }
}

class WaterfallChart {
    constructor(canvasId) {
        this.canvas = document.getElementById(canvasId);
        this.ctx = this.canvas.getContext('2d');
        this.frequencyBins = 512;
        this.maxHistoryLines = 300;
        this.maxFreq = 25600;
        this.isPaused = false;

        this.chunks = [];
        this.chunkSize = 50;
        this.currentChunk = [];
        this.totalLines = 0;

        this.maxChunks = Math.ceil(this.maxHistoryLines / this.chunkSize) + 2;

        this.useWorker = typeof OffscreenCanvas !== 'undefined';
        this.worker = null;
        this.renderPending = false;

        if (this.useWorker) {
            this.worker = new WaterfallWorker();
            this.worker.onFrame = (bitmap) => {
                this.ctx.drawImage(bitmap, 0, 0, this.width, this.height);
                bitmap.close();
                this.renderPending = false;
            };
        }

        this.resize();
        window.addEventListener('resize', () => this.resize());
    }

    resize() {
        const rect = this.canvas.parentElement.getBoundingClientRect();
        const dpr = window.devicePixelRatio || 1;
        this.canvas.width = rect.width * dpr;
        this.canvas.height = rect.height * dpr;
        this.canvas.style.width = rect.width + 'px';
        this.canvas.style.height = rect.height + 'px';
        this.width = rect.width * dpr;
        this.height = rect.height * dpr;

        if (this.useWorker && this.worker) {
            this.worker.init(this.width, this.height);
        }
    }

    addSpectrum(spectrumData) {
        if (this.isPaused) return;

        let line;
        if (spectrumData.length !== this.frequencyBins) {
            line = new Float32Array(this.frequencyBins);
            const ratio = spectrumData.length / this.frequencyBins;
            for (let i = 0; i < this.frequencyBins; i++) {
                const srcIdx = Math.floor(i * ratio);
                const nextIdx = Math.min(srcIdx + 1, spectrumData.length - 1);
                const frac = (i * ratio) - srcIdx;
                line[i] = spectrumData[srcIdx] * (1 - frac) + spectrumData[nextIdx] * frac;
            }
        } else {
            line = new Float32Array(spectrumData);
        }

        this.currentChunk.push(line);
        this.totalLines++;

        if (this.currentChunk.length >= this.chunkSize) {
            this.chunks.unshift(this.currentChunk);
            this.currentChunk = [];

            while (this.chunks.length > this.maxChunks) {
                this.chunks.pop();
            }

            this.compactOldChunks();
        }

        this.scheduleRender();
    }

    compactOldChunks() {
        if (this.chunks.length <= 4) return;

        for (let i = 3; i < this.chunks.length; i++) {
            const chunk = this.chunks[i];
            if (chunk.length > this.chunkSize / 2) {
                const halfLen = Math.floor(chunk.length / 2);
                const compacted = [];
                for (let j = 0; j < halfLen; j++) {
                    const line1 = chunk[j * 2];
                    const line2 = (j * 2 + 1 < chunk.length) ? chunk[j * 2 + 1] : line1;
                    const merged = new Float32Array(this.frequencyBins);
                    for (let k = 0; k < this.frequencyBins; k++) {
                        merged[k] = (line1[k] + line2[k]) / 2;
                    }
                    compacted.push(merged);
                }
                this.chunks[i] = compacted;
            }
        }
    }

    scheduleRender() {
        if (this.renderPending) return;
        this.renderPending = true;
        requestAnimationFrame(() => this.render());
    }

    getVisibleData() {
        const allLines = [];

        if (this.currentChunk.length > 0) {
            for (let i = this.currentChunk.length - 1; i >= 0; i--) {
                allLines.push(this.currentChunk[i]);
            }
        }

        for (const chunk of this.chunks) {
            for (const line of chunk) {
                allLines.push(line);
            }
            if (allLines.length >= this.maxHistoryLines) break;
        }

        return allLines.slice(0, this.maxHistoryLines);
    }

    render() {
        const lines = this.getVisibleData();

        if (lines.length === 0) {
            this.ctx.fillStyle = '#0a0f1a';
            this.ctx.fillRect(0, 0, this.width, this.height);
            this.ctx.fillStyle = '#555';
            this.ctx.font = '14px sans-serif';
            this.ctx.textAlign = 'center';
            this.ctx.fillText('等待数据...', this.width / 2, this.height / 2);
            this.renderPending = false;
            return;
        }

        let maxVal = 0;
        let minVal = Infinity;
        for (let i = 0; i < Math.min(lines.length, 50); i++) {
            for (let j = 0; j < lines[i].length; j++) {
                if (lines[i][j] > maxVal) maxVal = lines[i][j];
                if (lines[i][j] < minVal) minVal = lines[i][j];
            }
        }
        if (maxVal === minVal) maxVal = minVal + 1;

        const chartLeft = 60;
        const chartTop = 20;
        const chartWidth = this.width - chartLeft - 60;
        const chartHeight = this.height - chartTop - 50;

        if (this.useWorker && this.worker) {
            this.worker.render({
                type: 'render',
                chunks: this.chunks,
                frequencyBins: this.frequencyBins,
                maxLines: this.maxHistoryLines,
                minVal, maxVal,
                chartLeft, chartTop, chartWidth, chartHeight,
                maxFreq: this.maxFreq
            });
        } else {
            this.renderFallback(lines, minVal, maxVal, chartLeft, chartTop, chartWidth, chartHeight);
            this.renderPending = false;
        }
    }

    renderFallback(lines, minVal, maxVal, chartLeft, chartTop, chartWidth, chartHeight) {
        const ctx = this.ctx;
        ctx.fillStyle = '#0a0f1a';
        ctx.fillRect(0, 0, this.width, this.height);

        const lineHeight = chartHeight / this.maxHistoryLines;
        const valRange = (maxVal - minVal) || 1;

        const imageData = ctx.createImageData(chartWidth, chartHeight);
        const pixels = imageData.data;

        for (let lineIdx = 0; lineIdx < lines.length; lineIdx++) {
            const line = lines[lineIdx];
            const y = Math.floor(lineIdx * lineHeight);
            const lineH = Math.max(1, Math.ceil(lineHeight));

            for (let px = 0; px < chartWidth; px++) {
                const freqIdx = Math.floor(px / chartWidth * line.length);
                const val = line[freqIdx] || 0;
                const normalized = (val - minVal) / valRange;

                let r, g, b;
                if (normalized < 0.25) {
                    const t = normalized / 0.25;
                    r = 0; g = Math.floor(t * 128); b = Math.floor(64 + t * 191);
                } else if (normalized < 0.5) {
                    const t = (normalized - 0.25) / 0.25;
                    r = Math.floor(t * 128); g = Math.floor(128 + t * 127); b = Math.floor(255 - t * 128);
                } else if (normalized < 0.75) {
                    const t = (normalized - 0.5) / 0.25;
                    r = Math.floor(128 + t * 127); g = Math.floor(255 - t * 128); b = Math.floor(127 - t * 127);
                } else {
                    const t = (normalized - 0.75) / 0.25;
                    r = 255; g = Math.floor(127 - t * 127); b = Math.floor(t * 64);
                }

                for (let py = y; py < y + lineH && py < chartHeight; py++) {
                    const pidx = (py * chartWidth + px) * 4;
                    pixels[pidx] = r;
                    pixels[pidx + 1] = g;
                    pixels[pidx + 2] = b;
                    pixels[pidx + 3] = 255;
                }
            }
        }

        ctx.putImageData(imageData, chartLeft, chartTop);

        ctx.fillStyle = '#888';
        ctx.font = '12px sans-serif';
        ctx.textAlign = 'center';
        for (let i = 0; i <= 5; i++) {
            const freq = (i / 5) * this.maxFreq;
            const x = chartLeft + (i / 5) * chartWidth;
            ctx.fillText((freq / 1000).toFixed(0) + ' kHz', x, chartTop + chartHeight + 20);
        }

        ctx.strokeStyle = '#333';
        ctx.lineWidth = 2;
        ctx.strokeRect(chartLeft, chartTop, chartWidth, chartHeight);
    }

    addTestData() {
        const spectrum = new Float32Array(this.frequencyBins);
        const time = Date.now() * 0.001;

        for (let i = 0; i < this.frequencyBins; i++) {
            const freq = (i / this.frequencyBins) * this.maxFreq;
            let amplitude = 0.1 + Math.random() * 0.1;

            const rotFreq = 2;
            for (let h = 1; h <= 10; h++) {
                const center = rotFreq * h;
                const width = 2;
                const peak = Math.exp(-Math.pow((freq - center) / width, 2));
                amplitude += peak * 0.5 / h;
            }

            const bladeFreq = rotFreq * 15;
            for (let h = 1; h <= 5; h++) {
                const center = bladeFreq * h;
                const width = 10;
                const peak = Math.exp(-Math.pow((freq - center) / width, 2));
                amplitude += peak * 0.8 / h;
            }

            if (Math.random() > 0.7) {
                const cavFreq = 5000 + Math.sin(time) * 2000;
                const width = 500;
                const peak = Math.exp(-Math.pow((freq - cavFreq) / width, 2));
                amplitude += peak * (0.3 + Math.sin(time * 0.5) * 0.2);
            }

            amplitude = Math.max(0, Math.min(1, amplitude));
            spectrum[i] = amplitude;
        }

        this.addSpectrum(spectrum);
    }

    clear() {
        this.chunks = [];
        this.currentChunk = [];
        this.totalLines = 0;
        this.ctx.fillStyle = '#0a0f1a';
        this.ctx.fillRect(0, 0, this.width, this.height);
    }

    pause() {
        this.isPaused = !this.isPaused;
        return this.isPaused;
    }

    setMaxFreq(freq) {
        this.maxFreq = freq;
    }

    destroy() {
        if (this.worker) {
            this.worker.destroy();
        }
    }
}
