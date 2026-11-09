#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <thread>

#include <db/DatabaseManager.hpp>
#include <log/Log.hpp>
#include <pki/CertificateAuthority.hpp>

namespace pgdemo::server {

/**
 * @brief Signs the CSRs that device processes submit, one at a time, on its own thread.
 *
 * The listener thread only calls `submit()` on `enrollment_csr`. The worker
 * reads the CSR (as demo_server), signs it with the CA key and records the
 * certificate through `issue_device_certificate()`; a CSR that fails a check
 * is refused through `reject_enrollment()` with the `PkiError` code. Either
 * way PostgreSQL fires `enrollment_events`.
 */
class CertificateSigner
{
public:
    /**
     * @param db       server database session
     * @param ca       the CA whose key signs
     * @param validity lifetime of issued certificates
     */
    CertificateSigner(std::shared_ptr<db::DatabaseManager> db, std::shared_ptr<pki::CertificateAuthority> ca,
                      std::chrono::seconds validity)
        : db_(std::move(db))
        , ca_(std::move(ca))
        , validity_(validity)
        , worker_([this](std::stop_token st) { run(st); })
    {
    }

    ~CertificateSigner()
    {
        worker_.request_stop();
        cv_.notify_all();
    }

    CertificateSigner(const CertificateSigner&) = delete;
    CertificateSigner& operator=(const CertificateSigner&) = delete;

    /// Queue a request whose status is CSR_SUBMITTED. Any thread.
    void submit(std::int64_t request_id)
    {
        {
            std::lock_guard lock(mtx_);
            queue_.push_back(request_id);
        }
        cv_.notify_one();
    }

    /// The CA certificate and its fingerprint.
    [[nodiscard]] const pki::CertificateAuthority& ca() const noexcept { return *ca_; }

private:
    void run(std::stop_token st)
    {
        for (;;)
        {
            std::int64_t id = 0;
            {
                std::unique_lock lock(mtx_);
                cv_.wait(lock, st, [&] { return !queue_.empty(); });
                if (st.stop_requested()) return;
                id = queue_.front();
                queue_.pop_front();
            }
            sign(id);
        }
    }

    void sign(std::int64_t id)
    {
        try
        {
            const auto e = db_->fetch_csr(id);
            if (!e || e->status != "CSR_SUBMITTED" || !e->csr_pem) return;
            try
            {
                const auto cert = ca_->sign_csr(*e->csr_pem, e->name, validity_);
                const auto issued = db_->issue_certificate(
                    id, ca_->fingerprint(),
                    db::IssuedCertificate{cert.serial, cert.fingerprint, cert.subject_cn, cert.not_before,
                                          cert.not_after, cert.pem});
                log::info(kLog, std::format("request {}: {} certificate {} issued to {} (device_id {})", id,
                                            e->kind, cert.serial, e->name, issued.device_id));
            }
            catch (const pki::PkiError& err)
            {
                log::warn(kLog, std::format("request {}: CSR of {} refused: {} ({})", id, e->name, err.code,
                                            err.what()));
                db_->reject_enrollment(id, err.code, err.what());
            }
        }
        catch (const std::exception& err)
        {
            log::error(kLog, std::format("request {}: issuing failed: {}", id, err.what()));
            try
            {
                db_->reject_enrollment(id, "ISSUE_FAILED", err.what());
            }
            catch (const std::exception&)
            {
            }
        }
    }

    static constexpr std::string_view kLog = "ca";

    std::shared_ptr<db::DatabaseManager> db_;
    std::shared_ptr<pki::CertificateAuthority> ca_;
    std::chrono::seconds validity_;
    std::mutex mtx_;
    std::condition_variable_any cv_;
    std::deque<std::int64_t> queue_;
    std::jthread worker_;
};

} // namespace pgdemo::server
