"use client";

import { Loading } from "@/components/ui";
import { useRouter } from "next/navigation";
import { useEffect } from "react";

export default function AdminIndex() {
  const router = useRouter();
  useEffect(() => router.replace("/admin/devices/"), [router]);
  return <Loading />;
}
