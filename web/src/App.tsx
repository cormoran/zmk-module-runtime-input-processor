/**
 * ZMK Module Template - Main Application
 * Demonstrates custom RPC communication with a ZMK device
 */

import { useContext, useState, useEffect, useMemo, useCallback } from "react";
import "./App.css";
import { connect as serial_connect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import {
  ZMKConnection,
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import {
  Request,
  Response,
  InputProcessorInfo,
  Notification,
} from "./proto/cormoran/rip/custom";

// Custom subsystem identifier - must match firmware registration
export const SUBSYSTEM_IDENTIFIER = "cormoran_rip";

function App() {
  return (
    <div className="app">
      <header className="app-header">
        <h1>🔧 ZMK Runtime Input Processor</h1>
        <p>Configure input processor settings at runtime</p>
      </header>

      <ZMKConnection
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
              <button
                className="btn btn-primary"
                onClick={() => connect(serial_connect)}
              >
                🔌 Connect Serial
              </button>
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

  const subsystem = useMemo(
    () => zmkApp?.findSubsystem(SUBSYSTEM_IDENTIFIER),
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [zmkApp?.state.customSubsystems]
  );

  const callRPC = useCallback(
    async (request: Request): Promise<Response | null> => {
      if (!zmkApp?.state.connection || !subsystem) return null;
      try {
        const service = new ZMKCustomSubsystem(
          zmkApp.state.connection,
          subsystem.index
        );

        const payload = Request.encode(request).finish();
        const responsePayload = await service.callRPC(payload);

        if (responsePayload) {
          return Response.decode(responsePayload);
        }
      } catch (err) {
        console.error("RPC call failed:", err);
        throw err;
      }
      return null;
    },
    [zmkApp?.state.connection, subsystem]
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
      setError(
        `Failed to load processors: ${err instanceof Error ? err.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  }, [callRPC]);

  const updateProcessor = useCallback(async () => {
    if (selectedProcessorId === null) return;

    setIsLoading(true);
    setError(null);

    try {
      // Send separate requests for each parameter
      // Set scale multiplier
      const mulRequest = Request.create({
        setScaleMultiplier: {
          id: selectedProcessorId,
          value: scaleMultiplier,
        },
      });
      const mulResp = await callRPC(mulRequest);
      if (mulResp?.error) {
        setError(mulResp.error.message);
        setIsLoading(false);
        return;
      }

      // Set scale divisor
      const divRequest = Request.create({
        setScaleDivisor: {
          id: selectedProcessorId,
          value: scaleDivisor,
        },
      });
      const divResp = await callRPC(divRequest);
      if (divResp?.error) {
        setError(divResp.error.message);
        setIsLoading(false);
        return;
      }

      // Set rotation
      const rotRequest = Request.create({
        setRotation: {
          id: selectedProcessorId,
          value: rotationDegrees,
        },
      });
      const rotResp = await callRPC(rotRequest);
      if (rotResp?.error) {
        setError(rotResp.error.message);
        setIsLoading(false);
        return;
      }

      // Set temp-layer enabled
      const enabledRequest = Request.create({
        setTempLayerEnabled: {
          id: selectedProcessorId,
          enabled: tempLayerEnabled,
        },
      });
      const enabledResp = await callRPC(enabledRequest);
      if (enabledResp?.error) {
        setError(enabledResp.error.message);
        setIsLoading(false);
        return;
      }

      // Set temp-layer layer
      const layerRequest = Request.create({
        setTempLayerLayer: {
          id: selectedProcessorId,
          layer: tempLayerLayer,
        },
      });
      const layerResp = await callRPC(layerRequest);
      if (layerResp?.error) {
        setError(layerResp.error.message);
        setIsLoading(false);
        return;
      }

      // Set temp-layer activation delay
      const actDelayRequest = Request.create({
        setTempLayerActivationDelay: {
          id: selectedProcessorId,
          activationDelayMs: tempLayerActivationDelay,
        },
      });
      const actDelayResp = await callRPC(actDelayRequest);
      if (actDelayResp?.error) {
        setError(actDelayResp.error.message);
        setIsLoading(false);
        return;
      }

      // Set temp-layer deactivation delay
      const deactDelayRequest = Request.create({
        setTempLayerDeactivationDelay: {
          id: selectedProcessorId,
          deactivationDelayMs: tempLayerDeactivationDelay,
        },
      });
      const deactDelayResp = await callRPC(deactDelayRequest);
      if (deactDelayResp?.error) {
        setError(deactDelayResp.error.message);
        setIsLoading(false);
        return;
      }

      // Updates will come via notifications
    } catch (err) {
      setError(
        `Failed to update processor: ${err instanceof Error ? err.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  }, [
    callRPC,
    selectedProcessorId,
    scaleMultiplier,
    scaleDivisor,
    rotationDegrees,
    tempLayerEnabled,
    tempLayerLayer,
    tempLayerActivationDelay,
    tempLayerDeactivationDelay,
  ]);

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
      }
    },
    [processors]
  );

  useEffect(() => {
    if (subsystem) {
      loadProcessors();
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [subsystem]);

  // Subscribe to notifications for processor changes
  useEffect(() => {
    if (!zmkApp || !subsystem) return;

    const unsubscribe = zmkApp.onNotification({
      type: "custom",
      subsystemIndex: subsystem.index,
      callback: (notification) => {
        try {
          // notification.payload contains the encoded Notification message
          const decoded = Notification.decode(notification.payload);
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
            if (selectedProcessorId === proc.id) {
              setScaleMultiplier(proc.scaleMultiplier);
              setScaleDivisor(proc.scaleDivisor);
              setRotationDegrees(proc.rotationDegrees);
              setTempLayerEnabled(proc.tempLayerEnabled);
              setTempLayerLayer(proc.tempLayerLayer);
              setTempLayerActivationDelay(proc.tempLayerActivationDelayMs);
              setTempLayerDeactivationDelay(proc.tempLayerDeactivationDelayMs);
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
            }
          }
        } catch (err) {
          console.error("Failed to decode notification:", err);
        }
      },
    });

    return unsubscribe;
  }, [zmkApp, subsystem, selectedProcessorId]);

  if (!zmkApp) return null;

  if (!subsystem) {
    return (
      <section className="card">
        <div className="warning-message">
          <p>
            ⚠️ Subsystem "{SUBSYSTEM_IDENTIFIER}" not found. Make sure your
            firmware includes the runtime input processor module.
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

        <div style={{ marginBottom: "1rem" }}>
          <button
            className="btn btn-primary"
            onClick={loadProcessors}
            disabled={isLoading}
          >
            {isLoading ? "⏳ Loading..." : "🔄 Refresh List"}
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
                <input
                  id="temp-layer"
                  type="number"
                  min="0"
                  max="15"
                  value={tempLayerLayer}
                  onChange={(e) =>
                    setTempLayerLayer(parseInt(e.target.value) || 0)
                  }
                />
                <div
                  style={{
                    fontSize: "0.85em",
                    color: "#666",
                    marginTop: "0.25rem",
                  }}
                >
                  Layer number to activate (0-15)
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

          <button
            className="btn btn-primary"
            onClick={updateProcessor}
            disabled={isLoading}
          >
            {isLoading ? "⏳ Applying..." : "✅ Apply Settings"}
          </button>
        </section>
      )}
    </>
  );
}

export default App;
