import { act, render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import { InputProcessorManager } from "../src/App";
import {
  InputProcessorInfo,
  InertiaStopReason,
  Notification,
  Request,
  Response,
} from "../src/proto/cormoran/rip/custom";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  ...jest.requireActual("@zmkfirmware/zmk-studio-ts-client"),
  call_rpc: jest.fn(),
}));

it("turns inertia on and off while retaining the processor's threshold", async () => {
  const app = createConnectedMockZMKApp({ subsystems: ["cormoran_rip"] });
  const rpc = call_rpc as jest.Mock;
  rpc.mockImplementation(async (_connection, request) =>
    request.core
      ? { core: { getLockState: 1 } }
      : { custom: { call: { payload: new Uint8Array([1]) } } }
  );

  const encode = jest.spyOn(Request, "encode").mockReturnValue({
    finish: () => new Uint8Array(),
  } as ReturnType<typeof Request.encode>);
  jest.spyOn(Response, "decode").mockReturnValue(Response.create({}));
  const processor = InputProcessorInfo.create({
    id: 1,
    name: "scroll",
    scaleMultiplier: 1,
    scaleDivisor: 1,
    axisSnapThreshold: 100,
    axisSnapTimeoutMs: 1000,
    inertiaWindowMs: 200,
    inertiaIntervalMs: 20,
    inertiaThreshold: 12,
    inertiaEnabled: false,
    inertiaDecayPercent: 8,
    inertiaFastThreshold: 0,
    inertiaFastOutputPercent: 200,
  });
  const decode = jest
    .spyOn(Notification, "decode")
    .mockReturnValue(
      Notification.create({ inputProcessorChanged: { processor } })
    );

  render(
    <ZMKAppProvider value={app}>
      <InputProcessorManager />
    </ZMKAppProvider>
  );

  await waitFor(() =>
    expect(app.onNotification).toHaveBeenCalledWith(
      expect.objectContaining({ type: "custom" })
    )
  );
  const subscription = (app.onNotification as jest.Mock).mock.calls.find(
    ([entry]) => entry.type === "custom"
  )?.[0];
  expect(subscription).toBeDefined();
  act(() => subscription.callback({ payload: new Uint8Array() }));

  const user = userEvent.setup();
  const enabled = await screen.findByRole("checkbox", {
    name: "Enable Inertia",
  });
  expect(enabled).not.toBeChecked();
  const threshold = screen.getByRole("spinbutton", {
    name: "Input Threshold (when enabled):",
  });
  expect(threshold).toHaveValue(12);
  await user.clear(threshold);
  await user.type(threshold, "17");
  expect(threshold).toHaveValue(17);
  const fastThreshold = screen.getByRole("spinbutton", {
    name: "Fast Input Threshold:",
  });
  const fastOutput = screen.getByRole("spinbutton", {
    name: "Fast Output (%):",
  });
  await user.clear(fastThreshold);
  await user.type(fastThreshold, "30");
  await user.clear(fastOutput);
  await user.type(fastOutput, "250");

  const appliedThresholds = () =>
    encode.mock.calls
      .map(([request]) => request.setInertiaThreshold?.threshold)
      .filter((value): value is number => value !== undefined);
  const appliedEnabled = () =>
    encode.mock.calls
      .map(([request]) => request.setInertiaEnabled?.enabled)
      .filter((value): value is boolean => value !== undefined);

  await user.click(enabled);
  await user.click(screen.getByRole("button", { name: /Apply Settings/i }));
  await waitFor(() => expect(appliedThresholds()).toEqual([17]));
  await waitFor(() => expect(appliedEnabled()).toEqual([true]));
  await waitFor(() =>
    expect(
      encode.mock.calls.some(
        ([request]) => request.setInertiaFastThreshold?.threshold === 30
      )
    ).toBe(true)
  );
  await waitFor(() =>
    expect(
      encode.mock.calls.some(
        ([request]) => request.setInertiaFastOutputPercent?.percent === 250
      )
    ).toBe(true)
  );

  await user.click(enabled);
  expect(threshold).toHaveValue(17);
  await user.click(screen.getByRole("button", { name: /Apply Settings/i }));
  await waitFor(() => expect(appliedThresholds()).toEqual([17]));
  await waitFor(() => expect(appliedEnabled()).toEqual([true, false]));

  await user.click(enabled);
  expect(threshold).toHaveValue(17);
  await user.click(screen.getByRole("button", { name: /Apply Settings/i }));
  await waitFor(() => expect(appliedThresholds()).toEqual([17]));
  await waitFor(() => expect(appliedEnabled()).toEqual([true, false, true]));

  const area = screen.getByRole("region", {
    name: "Infinite scroll tuning area",
  });
  expect(area).not.toHaveClass("inertia-scroll-area-active");
  await user.click(
    screen.getByRole("checkbox", { name: "Show inertia activity" })
  );
  await waitFor(() =>
    expect(
      encode.mock.calls.some(
        ([request]) => request.setInertiaNotifications?.enabled === true
      )
    ).toBe(true)
  );
  decode.mockReturnValueOnce(
    Notification.create({
      inertiaStateChanged: { id: 1, active: true },
    })
  );
  const currentSubscription = (app.onNotification as jest.Mock).mock.calls
    .filter(([entry]) => entry.type === "custom")
    .at(-1)?.[0];
  act(() => currentSubscription.callback({ payload: new Uint8Array() }));
  expect(area).toHaveClass("inertia-scroll-area-active");
  expect(screen.getByText("Inertia: active")).toBeInTheDocument();

  decode.mockReturnValueOnce(
    Notification.create({
      inertiaFastInputChanged: { id: 1, fastInput: true },
    })
  );
  act(() => currentSubscription.callback({ payload: new Uint8Array() }));
  expect(area).toHaveClass("inertia-scroll-area-fast");
  expect(area).not.toHaveClass("inertia-scroll-area-active");
  expect(screen.getByText("Inertia: fast input")).toBeInTheDocument();

  decode.mockReturnValueOnce(
    Notification.create({
      inertiaStateChanged: {
        id: 1,
        active: false,
        stopReason: InertiaStopReason.INERTIA_STOP_REASON_SETTLED,
      },
    })
  );
  act(() => currentSubscription.callback({ payload: new Uint8Array() }));
  expect(area).not.toHaveClass("inertia-scroll-area-active");
  expect(area).not.toHaveClass("inertia-scroll-area-fast");
  expect(
    screen.getByText("Inertia: stopped (no more scroll output)")
  ).toBeInTheDocument();

  await user.click(screen.getByRole("checkbox", { name: "Vertical scroll" }));
  expect(area).toHaveStyle({ overflowY: "hidden" });
  await user.click(screen.getByRole("checkbox", { name: "Horizontal scroll" }));
  expect(area).toHaveStyle({ overflowX: "hidden" });
});
