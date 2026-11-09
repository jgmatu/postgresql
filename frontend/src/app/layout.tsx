import type { Metadata } from "next";
import type { ReactNode } from "react";
import "./globals.css";

export const metadata: Metadata = {
  title: "pgdemo · Panel de control",
  description: "Dispositivos, identidad y telemetría de pgdemo sobre PostgreSQL",
};

export default function RootLayout({ children }: Readonly<{ children: ReactNode }>) {
  return (
    <html lang="es" className="bg-white antialiased lg:bg-zinc-100">
      <body className="min-h-svh font-sans text-zinc-950">{children}</body>
    </html>
  );
}
