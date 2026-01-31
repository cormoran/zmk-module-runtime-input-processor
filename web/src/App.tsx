/**
 * ZMK Module Template - Main Application
 * Demonstrates custom RPC communication with a ZMK device
 */

import { useContext, useState, useEffect } from "react";
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
} from "./proto/zmk/template/custom";

// Custom subsystem identifier - must match firmware registration
export const SUBSYSTEM_IDENTIFIER = "zmk__template";

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
  const [selectedProcessor, setSelectedProcessor] = useState<string | null>(
    null
  );
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  // Form state
  const [scaleMultiplier, setScaleMultiplier] = useState<number>(1);
  const [scaleDivisor, setScaleDivisor] = useState<number>(1);
  const [rotationDegrees, setRotationDegrees] = useState<number>(0);

  const subsystem = zmkApp?.findSubsystem(SUBSYSTEM_IDENTIFIER);

  const callRPC = async (request: Request): Promise<Response | null> => {
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
  };

  const loadProcessors = async () => {
    setIsLoading(true);
    setError(null);

    try {
      const request = Request.create({
        listInputProcessors: {},
      });

      const resp = await callRPC(request);
      if (resp?.listInputProcessors) {
        setProcessors(resp.listInputProcessors.processors);
        if (
          resp.listInputProcessors.processors.length > 0 &&
          !selectedProcessor
        ) {
          const first = resp.listInputProcessors.processors[0];
          setSelectedProcessor(first.name);
          setScaleMultiplier(first.scaleMultiplier);
          setScaleDivisor(first.scaleDivisor);
          setRotationDegrees(first.rotationDegrees);
        }
      } else if (resp?.error) {
        setError(resp.error.message);
      }
    } catch (err) {
      setError(
        `Failed to load processors: ${err instanceof Error ? err.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  };

  const updateProcessor = async () => {
    if (!selectedProcessor) return;

    setIsLoading(true);
    setError(null);

    try {
      const request = Request.create({
        setInputProcessor: {
          name: selectedProcessor,
          scaleMultiplier,
          scaleDivisor,
          rotationDegrees,
        },
      });

      const resp = await callRPC(request);
      if (resp?.setInputProcessor?.processor) {
        const updated = resp.setInputProcessor.processor;
        setProcessors((prev) =>
          prev.map((p) => (p.name === updated.name ? updated : p))
        );
      } else if (resp?.error) {
        setError(resp.error.message);
      }
    } catch (err) {
      setError(
        `Failed to update processor: ${err instanceof Error ? err.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  };

  const selectProcessor = (name: string) => {
    const proc = processors.find((p) => p.name === name);
    if (proc) {
      setSelectedProcessor(name);
      setScaleMultiplier(proc.scaleMultiplier);
      setScaleDivisor(proc.scaleDivisor);
      setRotationDegrees(proc.rotationDegrees);
    }
  };

  useEffect(() => {
    if (subsystem) {
      loadProcessors();
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [subsystem]);

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
                key={proc.name}
                className={`processor-item ${selectedProcessor === proc.name ? "selected" : ""}`}
                onClick={() => selectProcessor(proc.name)}
                style={{
                  padding: "0.75rem",
                  margin: "0.5rem 0",
                  border: "1px solid #ccc",
                  borderRadius: "4px",
                  cursor: "pointer",
                  backgroundColor:
                    selectedProcessor === proc.name ? "#e3f2fd" : "transparent",
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
                </div>
              </div>
            ))}
          </div>
        )}
      </section>

      {selectedProcessor && (
        <section className="card">
          <h2>Configure: {selectedProcessor}</h2>

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

export function RPCTestSection() {
  const zmkApp = useContext(ZMKAppContext);
  const [inputValue, setInputValue] = useState<number>(42);
  const [response, setResponse] = useState<string | null>(null);
  const [isLoading, setIsLoading] = useState(false);

  if (!zmkApp) return null;

  const subsystem = zmkApp.findSubsystem(SUBSYSTEM_IDENTIFIER);

  // Send a sample request to the firmware
  const sendSampleRequest = async () => {
    if (!zmkApp.state.connection || !subsystem) return;

    setIsLoading(true);
    setResponse(null);

    try {
      const service = new ZMKCustomSubsystem(
        zmkApp.state.connection,
        subsystem.index
      );

      // Create the request using ts-proto
      const request = Request.create({
        sample: {
          value: inputValue,
        },
      });

      // Encode and send the request
      const payload = Request.encode(request).finish();
      const responsePayload = await service.callRPC(payload);

      if (responsePayload) {
        const resp = Response.decode(responsePayload);
        console.log("Decoded response:", resp);

        if (resp.sample) {
          setResponse(resp.sample.value);
        } else if (resp.error) {
          setResponse(`Error: ${resp.error.message}`);
        }
      }
    } catch (error) {
      console.error("RPC call failed:", error);
      setResponse(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  };

  if (!subsystem) {
    return (
      <section className="card">
        <div className="warning-message">
          <p>
            ⚠️ Subsystem "{SUBSYSTEM_IDENTIFIER}" not found. Make sure your
            firmware includes the template module.
          </p>
        </div>
      </section>
    );
  }

  return (
    <section className="card">
      <h2>RPC Test</h2>
      <p>Send a sample request to the firmware:</p>

      <div className="input-group">
        <label htmlFor="value-input">Value:</label>
        <input
          id="value-input"
          type="number"
          value={inputValue}
          onChange={(e) => setInputValue(parseInt(e.target.value) || 0)}
        />
      </div>

      <button
        className="btn btn-primary"
        disabled={isLoading}
        onClick={sendSampleRequest}
      >
        {isLoading ? "⏳ Sending..." : "📤 Send Request"}
      </button>

      {response && (
        <div className="response-box">
          <h3>Response from Firmware:</h3>
          <pre>{response}</pre>
        </div>
      )}
    </section>
  );
}

export default App;
