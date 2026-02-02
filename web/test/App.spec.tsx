/**
 * Tests for App component
 *
 * This test file demonstrates how to test a ZMK web application using
 * the react-zmk-studio test helpers. It serves as a reference implementation
 * for users of this template.
 */

import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { setupZMKMocks } from "@cormoran/zmk-studio-react-hook/testing";
import App from "../src/App";

// Mock the ZMK client
jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  create_rpc_connection: jest.fn(),
  call_rpc: jest.fn(),
}));

jest.mock("@zmkfirmware/zmk-studio-ts-client/transport/serial", () => ({
  connect: jest.fn(),
}));

describe("App Component", () => {
  describe("Basic Rendering", () => {
    it("should render the application header", () => {
      render(<App />);

      // Check for the main title
      expect(screen.getByText(/ZMK Module Template/i)).toBeInTheDocument();
      expect(screen.getByText(/Custom Studio RPC Demo/i)).toBeInTheDocument();
    });

    it("should render connection button when disconnected", () => {
      render(<App />);

      // Check for connection button in disconnected state
      expect(screen.getByText(/Connect Serial/i)).toBeInTheDocument();
    });

    it("should render footer", () => {
      render(<App />);

      // Check for footer text
      expect(screen.getByText(/Template Module/i)).toBeInTheDocument();
    });
  });

  describe("Connection Flow", () => {
    let mocks: ReturnType<typeof setupZMKMocks>;

    beforeEach(() => {
      mocks = setupZMKMocks();
    });

    it("should connect to device when connect button is clicked", async () => {
      // Set up successful connection mock
      mocks.mockSuccessfulConnection({
        deviceName: "Test Keyboard",
        subsystems: ["zmk__template"],
      });

      // Mock the serial connect function to return our mock transport
      const { connect: serial_connect } =
        await import("@zmkfirmware/zmk-studio-ts-client/transport/serial");
      (serial_connect as jest.Mock).mockResolvedValue(mocks.mockTransport);

      // Render the app
      render(<App />);

      // Verify initial disconnected state
      expect(screen.getByText(/Connect Serial/i)).toBeInTheDocument();

      // Click the connect button
      const user = userEvent.setup();
      const connectButton = screen.getByText(/Connect Serial/i);
      await user.click(connectButton);

      // Wait for connection to complete and verify connected state
      await waitFor(() => {
        expect(
          screen.getByText(/Connected to: Test Keyboard/i)
        ).toBeInTheDocument();
      });

      // Verify disconnect button is now available
      expect(screen.getByText(/Disconnect/i)).toBeInTheDocument();

      // Verify RPC test section is visible
      expect(screen.getByText(/RPC Test/i)).toBeInTheDocument();
    });
  });
});
