/**
 * ZMK Module Template - Main Application
 * Demonstrates custom RPC communication with a ZMK device
 */

import { useContext, useState, useEffect, useCallback, useRef } from "react";
import "./App.css";
import { connect as gattConnect } from "@zmkfirmware/zmk-studio-ts-client/transport/gatt";
import {
  ZMKConnection,
  ZMKAppContext,
  useStudioLockState,
  isUnlockRequiredError,
  isWebSerialSupported,
  isWebBluetoothSupported,
  useCustomSubsystem,
  connectSerial,
} from "@cormoran/zmk-studio-react-hook";
import {
  Request,
  Response,
  InputProcessorInfo,
  Notification,
  AxisSnapMode,
  WriteMode,
  InertiaStopReason,
} from "./proto/cormoran/rip/custom";

// Custom subsystem identifier - must match firmware registration
export const SUBSYSTEM_IDENTIFIER = "cormoran_rip";

export const GITHUB_REPO = "cormoran/zmk-module-runtime-input-processor";

// Always credits the original template project this module was built from,
// regardless of GITHUB_REPO above.
export const TEMPLATE_CREDIT_REPO = "cormoran/zmk-module-template";
const DEFAULT_INERTIA_THRESHOLD = 12;

const INERTIA_STOP_LABELS: Record<InertiaStopReason, string> = {
  [InertiaStopReason.INERTIA_STOP_REASON_UNSPECIFIED]: "",
  [InertiaStopReason.INERTIA_STOP_REASON_SETTLED]: "no more scroll output",
  [InertiaStopReason.INERTIA_STOP_REASON_REVERSE_INPUT]: "reverse input",
  [InertiaStopReason.INERTIA_STOP_REASON_LAYER_INACTIVE]: "layer deactivated",
  [InertiaStopReason.INERTIA_STOP_REASON_SETTINGS_CHANGED]: "settings changed",
  [InertiaStopReason.UNRECOGNIZED]: "unknown reason",
};

// Keep this codec stable. useCustomSubsystem() memoizes its RPC functions from
// the codec identity; recreating it while rendering would recreate the loaders
// below and turn their effects into a reload loop.
const RIP_CODEC = {
  encode: (request: Request) => Request.encode(request).finish(),
  decode: (payload: Uint8Array) => Response.decode(payload),
};

function App() {
  return (
    <div className="app">
      <header className="app-header">
        <h1>🔧 ZMK Runtime Input Processor</h1>
        <p>Configure input processor settings at runtime</p>
      </header>

      <ZMKConnection
        autoReconnect
        renderDisconnected={({ connect, isLoading, error }) => (
          <section className="card">
            <h2>Device Connection</h2>
            {isLoading && <p>⏳ Connecting...</p>}
            {error && (
              <div className="error-message">
                <p>🚨 {error}</p>
              </div>
            )}
            {!isLoading && (
              <>
                <div className="connect-buttons">
                  {isWebSerialSupported() && (
                    <button
                      className="btn btn-primary"
                      onClick={() => connect(connectSerial)}
                    >
                      🔌 Connect USB
                    </button>
                  )}
                  {isWebBluetoothSupported() && (
                    <button
                      className="btn btn-primary"
                      onClick={() => connect(gattConnect)}
                    >
                      📶 Connect Bluetooth
                    </button>
                  )}
                  {!isWebSerialSupported() && !isWebBluetoothSupported() && (
                    <div className="warning-message">
                      <p>
                        ⚠️ Web Serial and Web Bluetooth are unavailable here.
                        Use a Chromium-based browser (Chrome, Edge, ...) over
                        HTTPS or localhost to connect to your keyboard.
                      </p>
                    </div>
                  )}
                </div>
                {isWebBluetoothSupported() && (
                  <p className="hint-message">
                    📶 Not showing up? Some firmware only advertises the Studio
                    Bluetooth service once unlocked — press the unlock key (
                    <code>&amp;studio_unlock</code> behavior) on your keyboard,
                    then try connecting again.
                  </p>
                )}
              </>
            )}
          </section>
        )}
        renderConnected={({ disconnect, deviceName }) => (
          <>
            <section className="card">
              <h2>Device Connection</h2>
              <div className="device-info">
                <h3>✅ Connected to: {deviceName}</h3>
              </div>
              <button className="btn btn-secondary" onClick={disconnect}>
                Disconnect
              </button>
            </section>

            <InputProcessorManager />
          </>
        )}
      />

      <footer className="app-footer">
        <p>
          <strong>Runtime Input Processor Module</strong> - Configure pointing
          device behavior
        </p>
        <p>
          <a
            href={`https://github.com/${GITHUB_REPO}`}
            target="_blank"
            rel="noreferrer"
          >
            {GITHUB_REPO}
          </a>
        </p>
        <p className="template-credit">
          Built from{" "}
          <a
            href={`https://github.com/${TEMPLATE_CREDIT_REPO}`}
            target="_blank"
            rel="noreferrer"
          >
            {TEMPLATE_CREDIT_REPO}
          </a>{" "}
          - AI ready ZMK module template by{" "}
          <a
            href="https://github.com/cormoran"
            target="_blank"
            rel="noreferrer"
          >
            @cormoran
          </a>
        </p>
      </footer>
    </div>
  );
}

export function InputProcessorManager() {
  const zmkApp = useContext(ZMKAppContext);
  const [processors, setProcessors] = useState<InputProcessorInfo[]>([]);
  const [selectedProcessorId, setSelectedProcessorId] = useState<number | null>(
    null
  );
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [isUpdating, setIsUpdating] = useState(false);

  // Layer information
  const [layers, setLayers] = useState<Array<{ index: number; name: string }>>(
    []
  );

  // Form state
  const [scaleMultiplier, setScaleMultiplier] = useState<number>(1);
  const [scaleDivisor, setScaleDivisor] = useState<number>(1);
  const [rotationDegrees, setRotationDegrees] = useState<number>(0);

  // Temp-layer layer state
  const [tempLayerEnabled, setTempLayerEnabled] = useState<boolean>(false);
  const [tempLayerLayer, setTempLayerLayer] = useState<number>(0);
  const [tempLayerActivationDelay, setTempLayerActivationDelay] =
    useState<number>(100);
  const [tempLayerDeactivationDelay, setTempLayerDeactivationDelay] =
    useState<number>(500);

  // Active layers state
  const [activeLayers, setActiveLayers] = useState<number>(0);

  // Axis snap state
  const [axisSnapMode, setAxisSnapMode] = useState<AxisSnapMode>(
    AxisSnapMode.AXIS_SNAP_MODE_NONE
  );
  const [axisSnapThreshold, setAxisSnapThreshold] = useState<number>(100);
  const [axisSnapTimeout, setAxisSnapTimeout] = useState<number>(1000);

  // Code mapping state
  const [xyToScrollEnabled, setXyToScrollEnabled] = useState<boolean>(false);
  const [xySwapEnabled, setXySwapEnabled] = useState<boolean>(false);
  // Axis invert state
  const [xInvert, setXInvert] = useState<boolean>(false);
  const [yInvert, setYInvert] = useState<boolean>(false);
  // Inertia settings.
  const [inertiaWindowMs, setInertiaWindowMs] = useState<number>(200);
  const [inertiaIntervalMs, setInertiaIntervalMs] = useState<number>(20);
  const [inertiaEnabled, setInertiaEnabled] = useState<boolean>(true);
  const [inertiaThreshold, setInertiaThreshold] = useState<string>(
    String(DEFAULT_INERTIA_THRESHOLD)
  );
  const [inertiaDecayPercent, setInertiaDecayPercent] = useState<number>(8);
  const [inertiaNormalMaxOutput, setInertiaNormalMaxOutput] =
    useState<number>(0);
  const [inertiaFastThreshold, setInertiaFastThreshold] = useState<number>(0);
  const [inertiaFastOutputPercent, setInertiaFastOutputPercent] =
    useState<string>("200");
  const [inertiaNotificationsEnabled, setInertiaNotificationsEnabled] =
    useState(false);
  const [inertiaActive, setInertiaActive] = useState(false);
  const [inertiaFastInput, setInertiaFastInput] = useState(false);
  const [inertiaStopReason, setInertiaStopReason] = useState<InertiaStopReason>(
    InertiaStopReason.INERTIA_STOP_REASON_UNSPECIFIED
  );
  const [verticalScrollEnabled, setVerticalScrollEnabled] = useState(true);
  const [horizontalScrollEnabled, setHorizontalScrollEnabled] = useState(true);
  const scrollAreaRef = useRef<HTMLDivElement>(null);
  const scrollTileWidth = 640;
  const scrollTileHeight = 480;
  const centerScrollArea = useCallback(() => {
    if (scrollAreaRef.current) {
      scrollAreaRef.current.scrollLeft = scrollTileWidth;
      scrollAreaRef.current.scrollTop = scrollTileHeight;
    }
  }, []);
  const handleTuningScroll = useCallback(() => {
    const area = scrollAreaRef.current;
    if (!area) return;
    if (horizontalScrollEnabled) {
      if (area.scrollLeft < scrollTileWidth / 2)
        area.scrollLeft += scrollTileWidth;
      if (area.scrollLeft >= scrollTileWidth * 1.5)
        area.scrollLeft -= scrollTileWidth;
    } else {
      area.scrollLeft = scrollTileWidth;
    }
    if (verticalScrollEnabled) {
      if (area.scrollTop < scrollTileHeight / 2)
        area.scrollTop += scrollTileHeight;
      if (area.scrollTop >= scrollTileHeight * 1.5)
        area.scrollTop -= scrollTileHeight;
    } else {
      area.scrollTop = scrollTileHeight;
    }
  }, [horizontalScrollEnabled, verticalScrollEnabled]);
  useEffect(() => {
    centerScrollArea();
  }, [selectedProcessorId, centerScrollArea]);
  const loadInertiaThreshold = useCallback((proc: InputProcessorInfo) => {
    setInertiaEnabled(proc.inertiaEnabled);
    setInertiaThreshold(
      String(proc.inertiaThreshold || DEFAULT_INERTIA_THRESHOLD)
    );
  }, []);

  // Where "Apply Settings" stores values: persist to flash (default) or keep
  // in memory only (lost on reboot until saved). Mirrors the custom-settings
  // write modes.
  const [writeMode, setWriteMode] = useState<WriteMode>(
    WriteMode.WRITE_MODE_PERSIST
  );

  const { ready, subsystem, call } = useCustomSubsystem(
    SUBSYSTEM_IDENTIFIER,
    RIP_CODEC
  );
  // useZMKApp.findSubsystem() deliberately returns a fresh object. Depend on
  // the stable index rather than its object identity in effects below.
  const subsystemIndex = subsystem?.index ?? null;
  const { locked } = useStudioLockState();

  // Studio's unlock requirement is per-request: when a mutating/reading call
  // fails with UNLOCK_REQUIRED, remember how to retry it and surface the
  // unlock prompt below. Once the device reports it's unlocked again (see the
  // effect further down), the last such action is retried automatically --
  // this module's own `cormoran_rip` subsystem is unsecured today, but this
  // keeps the web UI working unmodified if it's ever secured.
  const [awaitingUnlock, setAwaitingUnlock] = useState(false);
  const pendingRetryRef = useRef<(() => void) | null>(null);

  const requestUnlockRetry = useCallback((retry: () => void) => {
    pendingRetryRef.current = retry;
    setAwaitingUnlock(true);
  }, []);

  const callRPC = useCallback(
    async (request: Request): Promise<Response | null> => {
      if (!ready) return null;
      try {
        return await call(request);
      } catch (err) {
        console.error("RPC call failed:", err);
        throw err;
      }
    },
    [ready, call]
  );

  const loadProcessors = useCallback(async () => {
    setIsLoading(true);
    setError(null);

    try {
      // Request list of input processors - notifications will be sent for each processor
      const request = Request.create({
        listInputProcessors: {},
      });

      const resp = await callRPC(request);
      if (resp?.error) {
        setError(resp.error.message);
      }
      // Response is empty - processors will arrive via notifications
    } catch (err) {
      if (isUnlockRequiredError(err)) {
        requestUnlockRetry(() => {
          void loadProcessors();
        });
        return;
      }
      setError(
        `Failed to load processors: ${err instanceof Error ? err.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  }, [callRPC, requestUnlockRetry]);

  const loadLayerInfo = useCallback(async () => {
    try {
      const request = Request.create({
        getLayerInfo: {},
      });

      const resp = await callRPC(request);
      if (resp?.getLayerInfo?.layers) {
        setLayers(resp.getLayerInfo.layers);
      } else if (resp?.error) {
        console.error("Failed to load layer info:", resp.error.message);
      }
    } catch (err) {
      if (isUnlockRequiredError(err)) {
        requestUnlockRetry(() => {
          void loadLayerInfo();
        });
        return;
      }
      console.error("Failed to load layer info:", err);
    }
  }, [callRPC, requestUnlockRetry]);

  const updateProcessor = useCallback(async () => {
    if (selectedProcessorId === null) return;

    const currentProcessor = processors.find(
      (p) => p.id === selectedProcessorId
    );
    if (!currentProcessor) return;

    setIsLoading(true);
    setError(null);
    setIsUpdating(true);

    try {
      // Only send requests for fields that have actually changed
      if (currentProcessor.scaleMultiplier !== scaleMultiplier) {
        const mulRequest = Request.create({
          setScaleMultiplier: {
            id: selectedProcessorId,
            writeMode,
            value: scaleMultiplier,
          },
        });
        const mulResp = await callRPC(mulRequest);
        if (mulResp?.error) {
          setError(mulResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      if (currentProcessor.scaleDivisor !== scaleDivisor) {
        const divRequest = Request.create({
          setScaleDivisor: {
            id: selectedProcessorId,
            writeMode,
            value: scaleDivisor,
          },
        });
        const divResp = await callRPC(divRequest);
        if (divResp?.error) {
          setError(divResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      if (currentProcessor.rotationDegrees !== rotationDegrees) {
        const rotRequest = Request.create({
          setRotation: {
            id: selectedProcessorId,
            writeMode,
            value: rotationDegrees,
          },
        });
        const rotResp = await callRPC(rotRequest);
        if (rotResp?.error) {
          setError(rotResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      if (currentProcessor.tempLayerEnabled !== tempLayerEnabled) {
        const enabledRequest = Request.create({
          setTempLayerEnabled: {
            id: selectedProcessorId,
            writeMode,
            enabled: tempLayerEnabled,
          },
        });
        const enabledResp = await callRPC(enabledRequest);
        if (enabledResp?.error) {
          setError(enabledResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      if (currentProcessor.tempLayerLayer !== tempLayerLayer) {
        const layerRequest = Request.create({
          setTempLayerLayer: {
            id: selectedProcessorId,
            writeMode,
            layer: tempLayerLayer,
          },
        });
        const layerResp = await callRPC(layerRequest);
        if (layerResp?.error) {
          setError(layerResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      if (
        currentProcessor.tempLayerActivationDelayMs !== tempLayerActivationDelay
      ) {
        const actDelayRequest = Request.create({
          setTempLayerActivationDelay: {
            id: selectedProcessorId,
            writeMode,
            activationDelayMs: tempLayerActivationDelay,
          },
        });
        const actDelayResp = await callRPC(actDelayRequest);
        if (actDelayResp?.error) {
          setError(actDelayResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      if (
        currentProcessor.tempLayerDeactivationDelayMs !==
        tempLayerDeactivationDelay
      ) {
        const deactDelayRequest = Request.create({
          setTempLayerDeactivationDelay: {
            id: selectedProcessorId,
            writeMode,
            deactivationDelayMs: tempLayerDeactivationDelay,
          },
        });
        const deactDelayResp = await callRPC(deactDelayRequest);
        if (deactDelayResp?.error) {
          setError(deactDelayResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      if (currentProcessor.activeLayers !== activeLayers) {
        const activeLayersRequest = Request.create({
          setActiveLayers: {
            id: selectedProcessorId,
            writeMode,
            layers: activeLayers,
          },
        });
        const activeLayersResp = await callRPC(activeLayersRequest);
        if (activeLayersResp?.error) {
          setError(activeLayersResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      if (currentProcessor.axisSnapMode !== axisSnapMode) {
        const axisSnapModeRequest = Request.create({
          setAxisSnapMode: {
            id: selectedProcessorId,
            writeMode,
            mode: axisSnapMode,
          },
        });
        const axisSnapModeResp = await callRPC(axisSnapModeRequest);
        if (axisSnapModeResp?.error) {
          setError(axisSnapModeResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      if (currentProcessor.axisSnapThreshold !== axisSnapThreshold) {
        const axisSnapThresholdRequest = Request.create({
          setAxisSnapThreshold: {
            id: selectedProcessorId,
            writeMode,
            threshold: axisSnapThreshold,
          },
        });
        const axisSnapThresholdResp = await callRPC(axisSnapThresholdRequest);
        if (axisSnapThresholdResp?.error) {
          setError(axisSnapThresholdResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      if (currentProcessor.axisSnapTimeoutMs !== axisSnapTimeout) {
        const axisSnapTimeoutRequest = Request.create({
          setAxisSnapTimeout: {
            id: selectedProcessorId,
            writeMode,
            timeoutMs: axisSnapTimeout,
          },
        });
        const axisSnapTimeoutResp = await callRPC(axisSnapTimeoutRequest);
        if (axisSnapTimeoutResp?.error) {
          setError(axisSnapTimeoutResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      if (currentProcessor.xyToScrollEnabled !== xyToScrollEnabled) {
        const xyToScrollRequest = Request.create({
          setXyToScrollEnabled: {
            id: selectedProcessorId,
            writeMode,
            enabled: xyToScrollEnabled,
          },
        });
        const xyToScrollResp = await callRPC(xyToScrollRequest);
        if (xyToScrollResp?.error) {
          setError(xyToScrollResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      if (currentProcessor.xInvert !== xInvert) {
        const xInvertRequest = Request.create({
          setXInvert: {
            id: selectedProcessorId,
            writeMode,
            invert: xInvert,
          },
        });
        const xInvertResp = await callRPC(xInvertRequest);
        if (xInvertResp?.error) {
          setError(xInvertResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      if (currentProcessor.xySwapEnabled !== xySwapEnabled) {
        const xySwapRequest = Request.create({
          setXySwapEnabled: {
            id: selectedProcessorId,
            writeMode,
            enabled: xySwapEnabled,
          },
        });
        const xySwapResp = await callRPC(xySwapRequest);
        if (xySwapResp?.error) {
          setError(xySwapResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      if (currentProcessor.yInvert !== yInvert) {
        const yInvertRequest = Request.create({
          setYInvert: {
            id: selectedProcessorId,
            writeMode,
            invert: yInvert,
          },
        });
        const yInvertResp = await callRPC(yInvertRequest);
        if (yInvertResp?.error) {
          setError(yInvertResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      if (currentProcessor.inertiaWindowMs !== inertiaWindowMs) {
        const inertiaWindowRequest = Request.create({
          setInertiaWindow: {
            id: selectedProcessorId,
            writeMode,
            windowMs: inertiaWindowMs,
          },
        });
        const inertiaWindowResp = await callRPC(inertiaWindowRequest);
        if (inertiaWindowResp?.error) {
          setError(inertiaWindowResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      if (currentProcessor.inertiaIntervalMs !== inertiaIntervalMs) {
        const inertiaIntervalRequest = Request.create({
          setInertiaInterval: {
            id: selectedProcessorId,
            writeMode,
            intervalMs: inertiaIntervalMs,
          },
        });
        const inertiaIntervalResp = await callRPC(inertiaIntervalRequest);
        if (inertiaIntervalResp?.error) {
          setError(inertiaIntervalResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      const effectiveInertiaThreshold = Math.min(
        65535,
        Math.max(1, parseInt(inertiaThreshold, 10) || DEFAULT_INERTIA_THRESHOLD)
      );
      if (currentProcessor.inertiaThreshold !== effectiveInertiaThreshold) {
        const inertiaThresholdRequest = Request.create({
          setInertiaThreshold: {
            id: selectedProcessorId,
            writeMode,
            threshold: effectiveInertiaThreshold,
          },
        });
        const inertiaThresholdResp = await callRPC(inertiaThresholdRequest);
        if (inertiaThresholdResp?.error) {
          setError(inertiaThresholdResp.error.message);
          setIsLoading(false);
          return;
        }
        setProcessors((previous) =>
          previous.map((proc) =>
            proc.id === selectedProcessorId
              ? { ...proc, inertiaThreshold: effectiveInertiaThreshold }
              : proc
          )
        );
      }

      if (currentProcessor.inertiaEnabled !== inertiaEnabled) {
        const response = await callRPC(
          Request.create({
            setInertiaEnabled: {
              id: selectedProcessorId,
              writeMode,
              enabled: inertiaEnabled,
            },
          })
        );
        if (response?.error) {
          setError(response.error.message);
          setIsLoading(false);
          return;
        }
        setProcessors((previous) =>
          previous.map((proc) =>
            proc.id === selectedProcessorId ? { ...proc, inertiaEnabled } : proc
          )
        );
      }

      if (currentProcessor.inertiaDecayPercent !== inertiaDecayPercent) {
        const inertiaDecayRequest = Request.create({
          setInertiaDecay: {
            id: selectedProcessorId,
            writeMode,
            decayPercent: inertiaDecayPercent,
          },
        });
        const inertiaDecayResp = await callRPC(inertiaDecayRequest);
        if (inertiaDecayResp?.error) {
          setError(inertiaDecayResp.error.message);
          setIsLoading(false);
          return;
        }
      }

      if (currentProcessor.inertiaNormalMaxOutput !== inertiaNormalMaxOutput) {
        const response = await callRPC(
          Request.create({
            setInertiaNormalMaxOutput: {
              id: selectedProcessorId,
              writeMode,
              maxOutput: inertiaNormalMaxOutput,
            },
          })
        );
        if (response?.error) {
          setError(response.error.message);
          return;
        }
        setProcessors((previous) =>
          previous.map((proc) =>
            proc.id === selectedProcessorId
              ? { ...proc, inertiaNormalMaxOutput }
              : proc
          )
        );
      }

      if (currentProcessor.inertiaFastThreshold !== inertiaFastThreshold) {
        const response = await callRPC(
          Request.create({
            setInertiaFastThreshold: {
              id: selectedProcessorId,
              writeMode,
              threshold: inertiaFastThreshold,
            },
          })
        );
        if (response?.error) {
          setError(response.error.message);
          return;
        }
      }

      const effectiveFastOutputPercent = Math.min(
        1000,
        Math.max(100, parseInt(inertiaFastOutputPercent, 10) || 200)
      );
      if (
        currentProcessor.inertiaFastOutputPercent !== effectiveFastOutputPercent
      ) {
        const response = await callRPC(
          Request.create({
            setInertiaFastOutputPercent: {
              id: selectedProcessorId,
              writeMode,
              percent: effectiveFastOutputPercent,
            },
          })
        );
        if (response?.error) {
          setError(response.error.message);
          return;
        }
      }

      // Updates will come via notifications
    } catch (err) {
      if (isUnlockRequiredError(err)) {
        requestUnlockRetry(() => {
          void updateProcessor();
        });
        return;
      }
      setError(
        `Failed to update processor: ${err instanceof Error ? err.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
      setIsUpdating(false);
    }
  }, [
    callRPC,
    requestUnlockRetry,
    processors,
    selectedProcessorId,
    scaleMultiplier,
    scaleDivisor,
    rotationDegrees,
    tempLayerEnabled,
    tempLayerLayer,
    tempLayerActivationDelay,
    tempLayerDeactivationDelay,
    activeLayers,
    axisSnapMode,
    axisSnapThreshold,
    axisSnapTimeout,
    xyToScrollEnabled,
    xySwapEnabled,
    xInvert,
    yInvert,
    inertiaWindowMs,
    inertiaIntervalMs,
    inertiaEnabled,
    inertiaThreshold,
    inertiaDecayPercent,
    inertiaNormalMaxOutput,
    inertiaFastThreshold,
    inertiaFastOutputPercent,
    writeMode,
  ]);

  // Save all / discard all / reset all - mirror the custom-settings operations
  // across every processor. After each, reload so the form reflects the result.
  const runAllSettingsOp = useCallback(
    async (request: Request, label: string) => {
      setIsLoading(true);
      setError(null);
      try {
        const resp = await callRPC(request);
        if (resp?.error) {
          setError(resp.error.message);
          return;
        }
        await loadProcessors();
      } catch (err) {
        setError(
          `Failed to ${label}: ${err instanceof Error ? err.message : "Unknown error"}`
        );
      } finally {
        setIsLoading(false);
      }
    },
    [callRPC, loadProcessors]
  );

  const saveAllSettings = useCallback(
    () => runAllSettingsOp(Request.create({ saveAllSettings: {} }), "save"),
    [runAllSettingsOp]
  );
  const discardAllSettings = useCallback(
    () =>
      runAllSettingsOp(Request.create({ discardAllSettings: {} }), "discard"),
    [runAllSettingsOp]
  );
  const resetAllSettings = useCallback(
    () => runAllSettingsOp(Request.create({ resetAllSettings: {} }), "reset"),
    [runAllSettingsOp]
  );

  const selectProcessor = useCallback(
    (id: number) => {
      const proc = processors.find((p) => p.id === id);
      if (proc) {
        setSelectedProcessorId(id);
        setScaleMultiplier(proc.scaleMultiplier);
        setScaleDivisor(proc.scaleDivisor);
        setRotationDegrees(proc.rotationDegrees);
        setTempLayerEnabled(proc.tempLayerEnabled);
        setTempLayerLayer(proc.tempLayerLayer);
        setTempLayerActivationDelay(proc.tempLayerActivationDelayMs);
        setTempLayerDeactivationDelay(proc.tempLayerDeactivationDelayMs);
        setActiveLayers(proc.activeLayers);
        setAxisSnapMode(proc.axisSnapMode);
        setAxisSnapThreshold(proc.axisSnapThreshold);
        setAxisSnapTimeout(proc.axisSnapTimeoutMs);
        setXyToScrollEnabled(proc.xyToScrollEnabled);
        setXySwapEnabled(proc.xySwapEnabled);
        setXInvert(proc.xInvert);
        setYInvert(proc.yInvert);
        setInertiaWindowMs(proc.inertiaWindowMs);
        setInertiaIntervalMs(proc.inertiaIntervalMs);
        loadInertiaThreshold(proc);
        setInertiaDecayPercent(proc.inertiaDecayPercent);
        setInertiaNormalMaxOutput(proc.inertiaNormalMaxOutput);
        setInertiaFastThreshold(proc.inertiaFastThreshold);
        setInertiaFastOutputPercent(
          String(proc.inertiaFastOutputPercent || 200)
        );
        setInertiaNotificationsEnabled(proc.inertiaNotificationsEnabled);
        setInertiaActive(proc.inertiaActive);
        setInertiaFastInput(false);
        setInertiaStopReason(InertiaStopReason.INERTIA_STOP_REASON_UNSPECIFIED);
      }
    },
    [processors, loadInertiaThreshold]
  );

  useEffect(() => {
    if (subsystemIndex === null) return;

    void loadProcessors();
    void loadLayerInfo();
  }, [subsystemIndex, loadProcessors, loadLayerInfo]);

  // Auto-retry once the device reports it's unlocked again -- covers the
  // common case where the user presses &studio_unlock after seeing the
  // prompt below without needing to click "Retry" themselves.
  useEffect(() => {
    if (awaitingUnlock && !locked && pendingRetryRef.current) {
      const retry = pendingRetryRef.current;
      pendingRetryRef.current = null;
      setAwaitingUnlock(false);
      retry();
    }
  }, [locked, awaitingUnlock]);

  // Subscribe to notifications for processor changes
  useEffect(() => {
    if (!zmkApp || subsystemIndex === null) return;

    const unsubscribe = zmkApp.onNotification({
      type: "custom",
      subsystemIndex,
      callback: (notification) => {
        try {
          // notification.payload contains the encoded Notification message
          const decoded = Notification.decode(notification.payload);
          if (
            decoded.inertiaStateChanged &&
            decoded.inertiaStateChanged.id === selectedProcessorId
          ) {
            setInertiaActive(decoded.inertiaStateChanged.active);
            setInertiaStopReason(decoded.inertiaStateChanged.stopReason);
            if (!decoded.inertiaStateChanged.active) setInertiaFastInput(false);
          }
          if (
            decoded.inertiaFastInputChanged &&
            decoded.inertiaFastInputChanged.id === selectedProcessorId
          ) {
            setInertiaFastInput(decoded.inertiaFastInputChanged.fastInput);
          }
          if (decoded.inputProcessorChanged?.processor) {
            const proc = decoded.inputProcessorChanged.processor;

            // Update or add processor to the list
            setProcessors((prev) => {
              const existingIndex = prev.findIndex((p) => p.id === proc.id);
              if (existingIndex >= 0) {
                // Update existing processor
                const updated = [...prev];
                updated[existingIndex] = proc;
                return updated;
              } else {
                // Add new processor
                return [...prev, proc];
              }
            });

            // If this is the currently selected processor, update form values
            // Skip updates if we're currently updating to prevent overwriting user changes
            if (selectedProcessorId === proc.id && !isUpdating) {
              setScaleMultiplier(proc.scaleMultiplier);
              setScaleDivisor(proc.scaleDivisor);
              setRotationDegrees(proc.rotationDegrees);
              setTempLayerEnabled(proc.tempLayerEnabled);
              setTempLayerLayer(proc.tempLayerLayer);
              setTempLayerActivationDelay(proc.tempLayerActivationDelayMs);
              setTempLayerDeactivationDelay(proc.tempLayerDeactivationDelayMs);
              setActiveLayers(proc.activeLayers);
              setAxisSnapMode(proc.axisSnapMode);
              setAxisSnapThreshold(proc.axisSnapThreshold);
              setAxisSnapTimeout(proc.axisSnapTimeoutMs);
              setXyToScrollEnabled(proc.xyToScrollEnabled);
              setXySwapEnabled(proc.xySwapEnabled);
              setXInvert(proc.xInvert);
              setYInvert(proc.yInvert);
              setInertiaWindowMs(proc.inertiaWindowMs);
              setInertiaIntervalMs(proc.inertiaIntervalMs);
              loadInertiaThreshold(proc);
              setInertiaDecayPercent(proc.inertiaDecayPercent);
              setInertiaNormalMaxOutput(proc.inertiaNormalMaxOutput);
              setInertiaFastThreshold(proc.inertiaFastThreshold);
              setInertiaFastOutputPercent(
                String(proc.inertiaFastOutputPercent || 200)
              );
              setInertiaNotificationsEnabled(proc.inertiaNotificationsEnabled);
              setInertiaActive(proc.inertiaActive);
              if (!proc.inertiaActive) setInertiaFastInput(false);
            }

            // If no processor is selected yet, select the first one
            if (selectedProcessorId === null) {
              setSelectedProcessorId(proc.id);
              setScaleMultiplier(proc.scaleMultiplier);
              setScaleDivisor(proc.scaleDivisor);
              setRotationDegrees(proc.rotationDegrees);
              setTempLayerEnabled(proc.tempLayerEnabled);
              setTempLayerLayer(proc.tempLayerLayer);
              setTempLayerActivationDelay(proc.tempLayerActivationDelayMs);
              setTempLayerDeactivationDelay(proc.tempLayerDeactivationDelayMs);
              setActiveLayers(proc.activeLayers);
              setAxisSnapMode(proc.axisSnapMode);
              setAxisSnapThreshold(proc.axisSnapThreshold);
              setAxisSnapTimeout(proc.axisSnapTimeoutMs);
              setXyToScrollEnabled(proc.xyToScrollEnabled);
              setXySwapEnabled(proc.xySwapEnabled);
              setXInvert(proc.xInvert);
              setYInvert(proc.yInvert);
              setInertiaWindowMs(proc.inertiaWindowMs);
              setInertiaIntervalMs(proc.inertiaIntervalMs);
              loadInertiaThreshold(proc);
              setInertiaDecayPercent(proc.inertiaDecayPercent);
              setInertiaNormalMaxOutput(proc.inertiaNormalMaxOutput);
              setInertiaFastThreshold(proc.inertiaFastThreshold);
              setInertiaFastOutputPercent(
                String(proc.inertiaFastOutputPercent || 200)
              );
              setInertiaNotificationsEnabled(proc.inertiaNotificationsEnabled);
              setInertiaActive(proc.inertiaActive);
              setInertiaFastInput(false);
            }
          }
        } catch (err) {
          console.error("Failed to decode notification:", err);
        }
      },
    });

    return unsubscribe;
  }, [
    zmkApp,
    subsystemIndex,
    selectedProcessorId,
    isUpdating,
    loadInertiaThreshold,
  ]);

  if (!zmkApp) return null;

  if (!subsystem) {
    return (
      <section className="card">
        <div className="warning-message">
          <p>
            ⚠️ Subsystem "{SUBSYSTEM_IDENTIFIER}" not found. Make sure your
            firmware includes the{" "}
            <a href={`https://github.com/${GITHUB_REPO}#readme`}>
              runtime input processor module
            </a>
            .
          </p>
        </div>
      </section>
    );
  }

  return (
    <>
      <section className="card">
        <h2>Input Processors</h2>
        {error && (
          <div className="error-message">
            <p>🚨 {error}</p>
          </div>
        )}

        {locked && (
          <div className="locked-banner">
            <p>🔒 ZMK Studio is locked.</p>
          </div>
        )}

        {awaitingUnlock && (
          <div className="unlock-prompt card">
            <p>
              🔒 ZMK Studio is locked. Press the unlock key (
              <code>&amp;studio_unlock</code> behavior) on your keyboard — the
              request will retry automatically.
            </p>
            <button
              className="btn btn-secondary"
              onClick={() => pendingRetryRef.current?.()}
            >
              Retry
            </button>
          </div>
        )}

        <div
          style={{
            marginBottom: "1rem",
            display: "flex",
            flexWrap: "wrap",
            gap: "0.5rem",
          }}
        >
          <button
            className="btn btn-primary"
            onClick={loadProcessors}
            disabled={isLoading || locked}
          >
            {isLoading ? "⏳ Loading..." : "🔄 Refresh List"}
          </button>
          <button
            className="btn btn-secondary"
            onClick={saveAllSettings}
            disabled={isLoading || locked}
            title="Persist every processor's current settings to flash"
          >
            💾 Save All
          </button>
          <button
            className="btn btn-secondary"
            onClick={discardAllSettings}
            disabled={isLoading || locked}
            title="Drop unsaved changes and reload the saved values"
          >
            ↩️ Discard All
          </button>
          <button
            className="btn btn-secondary"
            onClick={resetAllSettings}
            disabled={isLoading || locked}
            title="Reset every processor to its devicetree defaults"
          >
            🗑️ Reset All
          </button>
        </div>

        {processors.length === 0 && !isLoading && (
          <p>No input processors found. Configure them in your device tree.</p>
        )}

        {processors.length > 0 && (
          <div className="processor-list">
            {processors.map((proc) => (
              <div
                key={proc.id}
                className={`processor-item ${selectedProcessorId === proc.id ? "selected" : ""}`}
                onClick={() => selectProcessor(proc.id)}
                style={{
                  padding: "0.75rem",
                  margin: "0.5rem 0",
                  border: "1px solid #ccc",
                  borderRadius: "4px",
                  cursor: "pointer",
                  backgroundColor:
                    selectedProcessorId === proc.id ? "#e3f2fd" : "transparent",
                }}
              >
                <strong>{proc.name}</strong>
                <div
                  style={{
                    fontSize: "0.9em",
                    color: "#666",
                    marginTop: "0.25rem",
                  }}
                >
                  Scale: {proc.scaleMultiplier}/{proc.scaleDivisor} | Rotation:{" "}
                  {proc.rotationDegrees}°
                  {proc.tempLayerEnabled &&
                    ` | Temp-Layer: Layer ${proc.tempLayerLayer}`}
                </div>
              </div>
            ))}
          </div>
        )}
      </section>

      {selectedProcessorId !== null && (
        <section className="card">
          <h2>
            Configure:{" "}
            {processors.find((p) => p.id === selectedProcessorId)?.name}
          </h2>

          <div className="input-group">
            <label htmlFor="scale-multiplier">Scaling Multiplier:</label>
            <input
              id="scale-multiplier"
              type="number"
              min="1"
              value={scaleMultiplier}
              onChange={(e) =>
                setScaleMultiplier(parseInt(e.target.value) || 1)
              }
            />
          </div>

          <div className="input-group">
            <label htmlFor="scale-divisor">Scaling Divisor:</label>
            <input
              id="scale-divisor"
              type="number"
              min="1"
              value={scaleDivisor}
              onChange={(e) => setScaleDivisor(parseInt(e.target.value) || 1)}
            />
          </div>

          <div
            style={{
              marginBottom: "1rem",
              padding: "0.5rem",
              backgroundColor: "#f5f5f5",
              borderRadius: "4px",
            }}
          >
            <strong>Effective Scale:</strong>{" "}
            {(scaleMultiplier / scaleDivisor).toFixed(2)}x
            <div
              style={{ fontSize: "0.9em", color: "#666", marginTop: "0.25rem" }}
            >
              Examples: 2/1 = 2x faster, 1/2 = 0.5x slower
            </div>
          </div>

          <div className="input-group">
            <label htmlFor="rotation">Rotation (degrees):</label>
            <input
              id="rotation"
              type="number"
              min="-360"
              max="360"
              value={rotationDegrees}
              onChange={(e) =>
                setRotationDegrees(parseInt(e.target.value) || 0)
              }
            />
          </div>

          <hr style={{ margin: "1.5rem 0", border: "1px solid #e0e0e0" }} />

          <h3>Temp-Layer Layer</h3>
          <p style={{ fontSize: "0.9em", color: "#666", marginBottom: "1rem" }}>
            Automatically activate a layer when using the pointing device
          </p>

          <div className="input-group">
            <label htmlFor="temp-layer-enabled">
              <input
                id="temp-layer-enabled"
                type="checkbox"
                checked={tempLayerEnabled}
                onChange={(e) => setTempLayerEnabled(e.target.checked)}
                style={{ marginRight: "0.5rem" }}
              />
              Enable Temp-Layer Layer
            </label>
          </div>

          {tempLayerEnabled && (
            <>
              <div className="input-group">
                <label htmlFor="temp-layer">Target Layer:</label>
                <select
                  id="temp-layer"
                  value={tempLayerLayer}
                  onChange={(e) =>
                    setTempLayerLayer(parseInt(e.target.value) || 0)
                  }
                  style={{ padding: "0.5rem", fontSize: "1rem" }}
                >
                  {layers.length > 0
                    ? layers.map((layer) => (
                        <option key={layer.index} value={layer.index}>
                          {layer.name} (Layer {layer.index})
                        </option>
                      ))
                    : Array.from({ length: 16 }, (_, i) => i).map((i) => (
                        <option key={i} value={i}>
                          Layer {i}
                        </option>
                      ))}
                </select>
                <div
                  style={{
                    fontSize: "0.85em",
                    color: "#666",
                    marginTop: "0.25rem",
                  }}
                >
                  Layer to activate when using pointing device
                </div>
              </div>

              <div className="input-group">
                <label htmlFor="activation-delay">Activation Delay (ms):</label>
                <input
                  id="activation-delay"
                  type="number"
                  min="0"
                  max="5000"
                  step="10"
                  value={tempLayerActivationDelay}
                  onChange={(e) =>
                    setTempLayerActivationDelay(parseInt(e.target.value) || 0)
                  }
                />
                <div
                  style={{
                    fontSize: "0.85em",
                    color: "#666",
                    marginTop: "0.25rem",
                  }}
                >
                  Delay before activating layer (0-5000ms)
                </div>
              </div>

              <div className="input-group">
                <label htmlFor="deactivation-delay">
                  Deactivation Delay (ms):
                </label>
                <input
                  id="deactivation-delay"
                  type="number"
                  min="0"
                  max="5000"
                  step="10"
                  value={tempLayerDeactivationDelay}
                  onChange={(e) =>
                    setTempLayerDeactivationDelay(parseInt(e.target.value) || 0)
                  }
                />
                <div
                  style={{
                    fontSize: "0.85em",
                    color: "#666",
                    marginTop: "0.25rem",
                  }}
                >
                  Delay before deactivating layer after input stops (0-5000ms)
                </div>
              </div>
            </>
          )}

          <hr style={{ margin: "1.5rem 0", border: "1px solid #e0e0e0" }} />

          <h3>Active Layers</h3>
          <p style={{ fontSize: "0.9em", color: "#666", marginBottom: "1rem" }}>
            Select which layers the processor should be active on. If no layers
            are selected (0), the processor works on all layers.
          </p>

          <div className="input-group">
            <label htmlFor="active-layers">Layer Bitmask (hex):</label>
            <input
              id="active-layers"
              type="text"
              value={`0x${activeLayers.toString(16).toUpperCase().padStart(8, "0")}`}
              onChange={(e) => {
                const val = e.target.value.replace(/^0x/i, "");
                const parsed = parseInt(val || "0", 16);
                if (!isNaN(parsed)) {
                  setActiveLayers(parsed);
                }
              }}
              style={{ fontFamily: "monospace" }}
            />
            <div
              style={{
                fontSize: "0.85em",
                color: "#666",
                marginTop: "0.25rem",
              }}
            >
              Bitmask: bit 0 = layer 0, bit 1 = layer 1, etc. (0x00000000 = all
              layers)
            </div>
          </div>

          <div
            style={{
              marginTop: "1rem",
              padding: "0.75rem",
              backgroundColor: "#f5f5f5",
              borderRadius: "4px",
            }}
          >
            <strong>Select Layers:</strong>
            <div
              style={{
                display: "grid",
                gridTemplateColumns: "repeat(auto-fill, minmax(200px, 1fr))",
                gap: "0.5rem",
                marginTop: "0.5rem",
              }}
            >
              {layers.length > 0 ? (
                layers.map((layer) => (
                  <label
                    key={layer.index}
                    style={{
                      display: "flex",
                      alignItems: "center",
                      fontSize: "0.9em",
                      cursor: "pointer",
                      padding: "0.25rem",
                    }}
                  >
                    <input
                      type="checkbox"
                      checked={(activeLayers & (1 << layer.index)) !== 0}
                      onChange={(e) => {
                        if (e.target.checked) {
                          setActiveLayers(activeLayers | (1 << layer.index));
                        } else {
                          setActiveLayers(activeLayers & ~(1 << layer.index));
                        }
                      }}
                      style={{ marginRight: "0.5rem" }}
                    />
                    {layer.name}
                  </label>
                ))
              ) : (
                <p style={{ fontSize: "0.9em", color: "#666" }}>
                  Loading layers...
                </p>
              )}
            </div>
          </div>

          <hr style={{ margin: "1.5rem 0", border: "1px solid #e0e0e0" }} />

          <h3>Axis Snapping</h3>
          <p style={{ fontSize: "0.9em", color: "#666", marginBottom: "1rem" }}>
            Lock scrolling to a specific axis. Movement on the other axis is
            suppressed unless it exceeds the threshold within the timeout
            window.
          </p>

          <div className="input-group">
            <label htmlFor="axis-snap-mode">Snap Mode:</label>
            <select
              id="axis-snap-mode"
              value={axisSnapMode}
              onChange={(e) =>
                setAxisSnapMode(parseInt(e.target.value) as AxisSnapMode)
              }
              style={{ padding: "0.5rem", fontSize: "1rem" }}
            >
              <option value={AxisSnapMode.AXIS_SNAP_MODE_NONE}>No Snap</option>
              <option value={AxisSnapMode.AXIS_SNAP_MODE_X}>
                Snap to X Axis
              </option>
              <option value={AxisSnapMode.AXIS_SNAP_MODE_Y}>
                Snap to Y Axis
              </option>
            </select>
            <div
              style={{
                fontSize: "0.85em",
                color: "#666",
                marginTop: "0.25rem",
              }}
            >
              Select which axis to lock movement to
            </div>
          </div>

          {axisSnapMode !== AxisSnapMode.AXIS_SNAP_MODE_NONE && (
            <>
              <div className="input-group">
                <label htmlFor="axis-snap-threshold">Unlock Threshold:</label>
                <input
                  id="axis-snap-threshold"
                  type="number"
                  min="0"
                  max="1000"
                  step="10"
                  value={axisSnapThreshold}
                  onChange={(e) =>
                    setAxisSnapThreshold(parseInt(e.target.value) || 0)
                  }
                />
                <div
                  style={{
                    fontSize: "0.85em",
                    color: "#666",
                    marginTop: "0.25rem",
                  }}
                >
                  Cross-axis movement required to unlock snap (0-1000)
                </div>
              </div>

              <div className="input-group">
                <label htmlFor="axis-snap-timeout">Timeout Window (ms):</label>
                <input
                  id="axis-snap-timeout"
                  type="number"
                  min="0"
                  max="5000"
                  step="100"
                  value={axisSnapTimeout}
                  onChange={(e) =>
                    setAxisSnapTimeout(parseInt(e.target.value) || 0)
                  }
                />
                <div
                  style={{
                    fontSize: "0.85em",
                    color: "#666",
                    marginTop: "0.25rem",
                  }}
                >
                  Time window for threshold check (0-5000ms)
                </div>
              </div>
            </>
          )}

          <hr style={{ margin: "1.5rem 0", border: "1px solid #e0e0e0" }} />

          <h3>Code Mapping</h3>
          <p style={{ fontSize: "0.9em", color: "#666", marginBottom: "1rem" }}>
            Configure input code mapping features for your pointing device
          </p>

          <div className="input-group">
            <label htmlFor="xy-to-scroll-enabled">
              <input
                id="xy-to-scroll-enabled"
                type="checkbox"
                checked={xyToScrollEnabled}
                onChange={(e) => setXyToScrollEnabled(e.target.checked)}
                style={{ marginRight: "0.5rem" }}
              />
              Enable XY-to-Scroll Mapping
            </label>
            <div
              style={{
                fontSize: "0.85em",
                color: "#666",
                marginTop: "0.25rem",
                marginLeft: "1.7rem",
              }}
            >
              Map X/Y input to horizontal/vertical scroll wheel events
            </div>
          </div>

          <div className="input-group">
            <label htmlFor="xy-swap-enabled">
              <input
                id="xy-swap-enabled"
                type="checkbox"
                checked={xySwapEnabled}
                onChange={(e) => setXySwapEnabled(e.target.checked)}
                style={{ marginRight: "0.5rem" }}
              />
              Enable XY-Swap
            </label>
            <div
              style={{
                fontSize: "0.85em",
                color: "#666",
                marginTop: "0.25rem",
                marginLeft: "1.7rem",
              }}
            >
              Swap X and Y axes (Note: XY-to-scroll takes precedence)
            </div>
          </div>
          <h3>Axis Inversion</h3>
          <p style={{ fontSize: "0.9em", color: "#666", marginBottom: "1rem" }}>
            Invert axis values to reverse input direction (e.g., 2 becomes -2)
          </p>

          <div className="input-group">
            <label htmlFor="x-invert">
              <input
                id="x-invert"
                type="checkbox"
                checked={xInvert}
                onChange={(e) => setXInvert(e.target.checked)}
                style={{ marginRight: "0.5rem" }}
              />
              Invert X Axis
            </label>
            <div
              style={{
                fontSize: "0.85em",
                color: "#666",
                marginTop: "0.25rem",
                marginLeft: "1.5rem",
              }}
            >
              Reverse horizontal input direction
            </div>
          </div>

          <div className="input-group">
            <label htmlFor="y-invert">
              <input
                id="y-invert"
                type="checkbox"
                checked={yInvert}
                onChange={(e) => setYInvert(e.target.checked)}
                style={{ marginRight: "0.5rem" }}
              />
              Invert Y Axis
            </label>
            <div
              style={{
                fontSize: "0.85em",
                color: "#666",
                marginTop: "0.25rem",
                marginLeft: "1.5rem",
              }}
            >
              Reverse vertical input direction
            </div>
          </div>

          <hr style={{ margin: "1.5rem 0", border: "1px solid #e0e0e0" }} />

          <h3>Inertia</h3>
          <p style={{ fontSize: "0.9em", color: "#666", marginBottom: "1rem" }}>
            After same-direction input reaches the threshold within one time
            window, the processor emits inertia. A strong trackball scroll
            raises the retained speed.
          </p>

          <div className="input-group">
            <label htmlFor="inertia-enabled">
              <input
                id="inertia-enabled"
                type="checkbox"
                checked={inertiaEnabled}
                onChange={(e) => setInertiaEnabled(e.target.checked)}
                style={{ marginRight: "0.5rem" }}
              />
              Enable Inertia
            </label>
            <div
              style={{
                fontSize: "0.85em",
                color: "#666",
                marginTop: "0.25rem",
              }}
            >
              Turning this off keeps the threshold below. Press Apply Settings
              to send the change.
            </div>
          </div>

          <div className="input-group">
            <label htmlFor="inertia-window-ms">Measurement Window (ms):</label>
            <input
              id="inertia-window-ms"
              type="number"
              min="1"
              max="60000"
              step="1"
              value={inertiaWindowMs}
              onChange={(e) =>
                setInertiaWindowMs(
                  Math.min(60000, Math.max(1, parseInt(e.target.value) || 1))
                )
              }
            />
            <div
              style={{
                fontSize: "0.85em",
                color: "#666",
                marginTop: "0.25rem",
              }}
            >
              Input is accumulated over this duration for triggering and sliding
              speed measurement.
            </div>
          </div>

          <div className="input-group">
            <label htmlFor="inertia-interval-ms">Output Interval (ms):</label>
            <input
              id="inertia-interval-ms"
              type="number"
              min="1"
              max="60000"
              step="1"
              value={inertiaIntervalMs}
              onChange={(e) =>
                setInertiaIntervalMs(
                  Math.min(60000, Math.max(1, parseInt(e.target.value) || 1))
                )
              }
            />
            <div
              style={{
                fontSize: "0.85em",
                color: "#666",
                marginTop: "0.25rem",
              }}
            >
              Inertia is emitted at this cadence. Each output is scaled by
              interval ÷ measurement window.
            </div>
          </div>

          <div className="input-group">
            <label htmlFor="inertia-threshold">
              Input Threshold (when enabled):
            </label>
            <input
              id="inertia-threshold"
              type="number"
              min="1"
              max="65535"
              step="1"
              value={inertiaThreshold}
              onChange={(e) => setInertiaThreshold(e.target.value)}
            />
            <div
              style={{
                fontSize: "0.85em",
                color: "#666",
                marginTop: "0.25rem",
              }}
            >
              {!inertiaEnabled
                ? "Inertia is off. This threshold will be used when enabled."
                : "Threshold is measured after scaling. Reverse input uses raw counts; its limit is set in firmware Kconfig."}
            </div>
          </div>

          <div className="input-group">
            <label htmlFor="inertia-decay-percent">
              Decay per Output Interval (%):
            </label>
            <input
              id="inertia-decay-percent"
              type="number"
              min="0"
              max="100"
              step="1"
              value={inertiaDecayPercent}
              onChange={(e) =>
                setInertiaDecayPercent(
                  Math.min(100, Math.max(0, parseInt(e.target.value) || 0))
                )
              }
            />
            <div
              style={{
                fontSize: "0.85em",
                color: "#666",
                marginTop: "0.25rem",
              }}
            >
              Percentage removed from the remaining speed after each output
              interval without new input, producing exponential decay. 0 keeps
              the speed; 100 stops after one more interval.
            </div>
          </div>

          <div className="input-group">
            <label htmlFor="inertia-normal-max-output">
              Normal Output Limit (per interval):
            </label>
            <input
              id="inertia-normal-max-output"
              type="number"
              min="0"
              max="32767"
              step="1"
              value={inertiaNormalMaxOutput}
              onChange={(e) =>
                setInertiaNormalMaxOutput(
                  Math.min(
                    32767,
                    Math.max(0, parseInt(e.target.value, 10) || 0)
                  )
                )
              }
            />
            <div
              style={{
                fontSize: "0.85em",
                color: "#666",
                marginTop: "0.25rem",
              }}
            >
              Caps each generated output in normal inertia. Zero is unlimited.
              Fast mode ignores this limit.
            </div>
          </div>

          <div className="input-group">
            <label htmlFor="inertia-fast-threshold">
              Fast Input Threshold:
            </label>
            <input
              id="inertia-fast-threshold"
              type="number"
              min="0"
              max="65535"
              step="1"
              value={inertiaFastThreshold}
              onChange={(e) =>
                setInertiaFastThreshold(
                  Math.min(
                    65535,
                    Math.max(0, parseInt(e.target.value, 10) || 0)
                  )
                )
              }
            />
            <div
              style={{
                fontSize: "0.85em",
                color: "#666",
                marginTop: "0.25rem",
              }}
            >
              Second-stage threshold in scaled input counts over the same
              window. Zero disables fast scrolling. The boost stays on until
              inertia ends.
            </div>
          </div>

          <div className="input-group">
            <label htmlFor="inertia-fast-output-percent">
              Fast Output (%):
            </label>
            <input
              id="inertia-fast-output-percent"
              type="number"
              min="100"
              max="1000"
              step="1"
              value={inertiaFastOutputPercent}
              onChange={(e) => setInertiaFastOutputPercent(e.target.value)}
            />
            <div
              style={{
                fontSize: "0.85em",
                color: "#666",
                marginTop: "0.25rem",
              }}
            >
              Multiplies generated inertia output after normal scale; 200%
              doubles it.
            </div>
          </div>

          <section className="inertia-tuning">
            <h3>Scroll tuning area</h3>
            <p>
              Move the trackball here to feel the scrolling response. The area
              repeats in every direction.
            </p>
            <div className="inertia-tuning-controls">
              <label>
                <input
                  type="checkbox"
                  checked={verticalScrollEnabled}
                  onChange={(event) =>
                    setVerticalScrollEnabled(event.target.checked)
                  }
                />{" "}
                Vertical scroll
              </label>
              <label>
                <input
                  type="checkbox"
                  checked={horizontalScrollEnabled}
                  onChange={(event) =>
                    setHorizontalScrollEnabled(event.target.checked)
                  }
                />{" "}
                Horizontal scroll
              </label>
              <label>
                <input
                  type="checkbox"
                  checked={inertiaNotificationsEnabled}
                  onChange={async (event) => {
                    if (selectedProcessorId === null) return;
                    const enabled = event.target.checked;
                    try {
                      const response = await callRPC(
                        Request.create({
                          setInertiaNotifications: {
                            id: selectedProcessorId,
                            enabled,
                          },
                        })
                      );
                      if (response?.error) {
                        setError(response.error.message);
                        return;
                      }
                      setInertiaNotificationsEnabled(enabled);
                      if (!enabled) {
                        setInertiaActive(false);
                        setInertiaFastInput(false);
                      }
                    } catch (error) {
                      setError(
                        error instanceof Error ? error.message : String(error)
                      );
                    }
                  }}
                />{" "}
                Show inertia activity
              </label>
            </div>
            <div
              ref={scrollAreaRef}
              className={`inertia-scroll-area${inertiaActive && inertiaNotificationsEnabled ? (inertiaFastInput ? " inertia-scroll-area-fast" : " inertia-scroll-area-active") : ""}`}
              role="region"
              aria-label="Infinite scroll tuning area"
              style={{
                overflowX: horizontalScrollEnabled ? "auto" : "hidden",
                overflowY: verticalScrollEnabled ? "auto" : "hidden",
              }}
              onScroll={handleTuningScroll}
            >
              <div className="inertia-scroll-grid">
                {Array.from({ length: 9 }, (_, index) => (
                  <div className="inertia-scroll-tile" key={index}>
                    {Array.from({ length: 8 }, (_, line) => (
                      <p key={line}>
                        ↕ ↔ Scroll test · line {line + 1} · Trackball inertia
                      </p>
                    ))}
                  </div>
                ))}
              </div>
            </div>
            <span className="inertia-activity-label" aria-live="polite">
              Inertia:{" "}
              {inertiaNotificationsEnabled
                ? inertiaActive
                  ? inertiaFastInput
                    ? "fast input"
                    : "active"
                  : `stopped${INERTIA_STOP_LABELS[inertiaStopReason] ? ` (${INERTIA_STOP_LABELS[inertiaStopReason]})` : ""}`
                : "activity display off"}
            </span>
          </section>

          <div className="form-group" style={{ marginTop: "1rem" }}>
            <label htmlFor="write-mode-select">Storage</label>
            <select
              id="write-mode-select"
              value={writeMode}
              onChange={(e) =>
                setWriteMode(Number(e.target.value) as WriteMode)
              }
            >
              <option value={WriteMode.WRITE_MODE_PERSIST}>
                Persist to flash (survives reboot)
              </option>
              <option value={WriteMode.WRITE_MODE_MEMORY}>
                Memory only (until saved / reboot)
              </option>
            </select>
            <div
              style={{
                fontSize: "0.85em",
                color: "#666",
                marginTop: "0.25rem",
              }}
            >
              "Memory only" changes stay until you press "Save All" or the
              keyboard reboots. Use "Discard All" to drop unsaved changes.
            </div>
          </div>

          <button
            className="btn btn-primary"
            onClick={updateProcessor}
            disabled={isLoading || locked}
          >
            {isLoading ? "⏳ Applying..." : "✅ Apply Settings"}
          </button>
        </section>
      )}
    </>
  );
}

export default App;
