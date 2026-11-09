import { AuthLayout } from "@/components/catalyst/auth-layout";
import { Heading } from "@/components/catalyst/heading";
import { Text, TextLink } from "@/components/catalyst/text";

export default function NotFound() {
  return (
    <AuthLayout>
      <div className="max-w-sm text-center" data-testid="not-found">
        <p className="text-sm font-semibold text-sky-600">404</p>
        <Heading className="mt-2">Página no encontrada</Heading>
        <Text className="mt-2">
          Esta ruta no existe en el panel. <TextLink href="/devices/">Volver a los dispositivos</TextLink>.
        </Text>
      </div>
    </AuthLayout>
  );
}
