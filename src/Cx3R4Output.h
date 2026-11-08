/*
 * R4 diagnostics exceed the 4 KiB in-band IOKit transport limit. XNU passes
 * those outputs through structureOutputDescriptor, NOT structureOutput.
 * The caller includes IOLib and IOUserClient before this file.
 * Original cx3-mlx4 work; see LICENSE.
 */
#ifndef CX3_R4_OUTPUT_H
#define CX3_R4_OUTPUT_H

class Cx3R4Output {
public:
    Cx3R4Output(IOExternalMethodArguments *args, size_t expected)
        : fBytes(expected), fData(0), fDescriptor(0), fStatus(kIOReturnBadArgument)
    {
        if (args->structureOutputDescriptor) {
            if (args->structureOutputSize != 0 ||
                args->structureOutputDescriptorSize != expected ||
                args->structureOutputDescriptor->getLength() != expected) return;
            fDescriptor = args->structureOutputDescriptor;
            fData = IOMalloc(expected);
            if (!fData) { fStatus = kIOReturnNoMemory; return; }
        } else {
            if (!args->structureOutput || args->structureOutputSize != expected ||
                args->structureOutputDescriptorSize != 0) return;
            fData = args->structureOutput;
        }
        memset(fData, 0, fBytes);
        fStatus = kIOReturnSuccess;
    }

    ~Cx3R4Output() { if (fDescriptor && fData) IOFree(fData, fBytes); }
    IOReturn status() const { return fStatus; }
    void *data() const { return fData; }
    IOReturn publish(IOReturn result)
    {
        if (fStatus != kIOReturnSuccess) return fStatus;
        if (fDescriptor && fDescriptor->writeBytes(0, fData, fBytes) != fBytes)
            return kIOReturnError;
        return result;
    }

private:
    Cx3R4Output(const Cx3R4Output &);
    Cx3R4Output &operator=(const Cx3R4Output &);
    size_t fBytes;
    void *fData;
    IOMemoryDescriptor *fDescriptor;  /* Borrowed for this synchronous call only. */
    IOReturn fStatus;
};

#endif
