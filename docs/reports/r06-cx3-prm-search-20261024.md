# CX3 HW2SW_EQ specification search

2026-10-24. Scope: determine whether public, ConnectX-3/ConnectX-3 Pro-specific programming documentation defines successful `HW2SW_EQ` completion as relinquishing queue ownership / completing outstanding writes, and specifies ordering before host reuse of EQ memory. This was a bounded primary-source search, not a hardware experiment.

## Verdict

**Not found in this bounded pass.** I did not retrieve public CX3/CX3-Pro-specific normative text saying that successful `HW2SW_EQ` means no device writes remain outstanding, or defining a DMA ordering point before EQ memory reuse. This does not establish that no such specification exists.

Therefore, the existing report's gap remains: `HW2SW_EQ` success is queue-return evidence under the observed driver convention, but it is not yet proven to be an all-DMA drain or memory-reuse fence. No global reset is required as a normal pre-handoff cleanup condition.

## Exact material retrieved

The only directly relevant first-party material retrieved was an NVIDIA Developer Forum thread asking how to obtain a ConnectX Ethernet PRM. NVIDIA staff replied:

> “Do you have a Mellanox customer login account with a design-in portfolio? If you do, you should be able to get this file via the following link: Contact Support for link”

The follow-up says that a company with a Mellanox Design-in profile should already have an account and assigned field engineer; otherwise, engage Mellanox Support to start the process. This supports the practical conclusion that this PRM is access-controlled; it does **not** answer the `HW2SW_EQ` semantics question.

Source: [NVIDIA Developer Forums — ConnectX®EN (MTNIC) PRM Document Number: DOC18348](https://forums.developer.nvidia.com/t/connectx-en-mtnic-prm-document-number-doc18348/208238). Retrieved 2026-10-24 using Tavily extract. The quoted lines are reproduced verbatim from the extracted page. The page concerns the ConnectX EN/MTNIC PRM and is used only as evidence about the access route, not as a CX3 command specification.

Existing project notes identify the relevant CX3 programming reference as the ConnectX PRM Rev 1.3 or later for command interface revision 0x3, while noting that it is not publicly available. That is project-held release-note evidence, not a newly retrieved copy of the PRM, and it contains no `HW2SW_EQ` completion or DMA-fence text in the reviewed material.

## Search scope and limits

Four targeted search queries were sent to NVIDIA-domain search, covering the exact `HW2SW_EQ` token with the ConnectX PRM, PRM revision 1.3, NVIDIA OFED documentation, and NVIDIA network documentation. The results did not expose the CX3 PRM or a CX3-specific normative definition. Search indexing is not exhaustive, so absence from these results is not evidence of nonexistence.

The official forum page was then extracted directly. No keyless search fallback was used; the prior keyless monthly-cap failure remains undisturbed.

## What remains established versus unknown

- **Existing source-code convention, not a new finding:** Linux/FreeBSD issue `HW2SW_EQ` before host-side EQ resource reclamation. As documented in [`r06-eq-revoke-contract.md`](r06-eq-revoke-contract.md), this shows queue handback followed by separate IRQ/tasklet cleanup; it does not define the CX3 firmware's DMA completion guarantee.
- **Unavailable/unproven:** whether a successful CX3 `HW2SW_EQ` response guarantees that every previously issued DMA write targeting the EQ ring/MTT has completed, and what ordering point (if any) makes those pages safe to reuse.
- **Out of scope as proof:** CX4/CX5 PRMs, generic PCIe behavior, driver sequencing, or lack of observed writes cannot settle the CX3-specific guarantee.

## Actionable next source

Obtain the ConnectX PRM Rev 1.3-or-later edition applicable to command interface revision 0x3 through NVIDIA/Mellanox Support (the route indicated by the official forum response), then inspect its `HW2SW_EQ` command definition, output/status semantics, and any EQ teardown or DMA ordering sections. Record the exact revision, page/section, and literal wording. Until then, retain the DMA-release predicate as explicitly unresolved; this source gap need not block offline software work that preserves the EQ ring when `HW2SW_EQ` fails.
