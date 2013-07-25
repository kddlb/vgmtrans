#include "stdafx.h"

#include <zlib.h>
#include "Root.h"
#include "DataSeg.h"
#include "PSFFile.h"
#include "types.h"

#define PSF_TAG_SIG             "[TAG]"
#define PSF_TAG_SIG_LEN         5

PSFFile::PSFFile(void)
{
	exeData = new DataSeg();
	exeCompData = new DataSeg();
	reservedData = new DataSeg();

	Clear();
}

PSFFile::PSFFile(RawFile* file)
{
	exeData = new DataSeg();
	exeCompData = new DataSeg();
	reservedData = new DataSeg();

	Load(file);
}

PSFFile::~PSFFile(void)
{
	delete exeData;
	delete exeCompData;
	delete reservedData;
}

bool PSFFile::Load(RawFile* file)
{
	Clear();

	// Check file size.
	UINT fileSize = file->size();
	if (fileSize < 0x10)
	{
		errorstr = L"PSF too small - likely corrupt";
		return false;
	}

	// Check PSF signature.
	BYTE psfSig[4];
	file->GetBytes(0, 4, psfSig);
	if (memcmp(psfSig, "PSF", 3) != 0)
	{
		errorstr = L"Invalid PSF signature";
		return false;
	}

	// Read the version number.
	version = psfSig[3];

	// Read the remaining header values.
	uint32_t reservedSize = file->GetWord(4);
	uint32_t exeSize = file->GetWord(8);
	exeCRC = file->GetWord(12);

	// Consistency check on section lengths.
	if( (reservedSize > fileSize) ||
		(exeSize > fileSize) ||
		((16+reservedSize+exeSize) > fileSize))
	{
		errorstr = L"PSF header is inconsistent";
		return false;
	}

	// Read compressed program section.
	BYTE* zexebuf = new BYTE[exeSize > 0 ? exeSize : 1];
	if (zexebuf == NULL)
	{
		errorstr = L"Out of memory reading the file";
		return false;
	}
	file->GetBytes(16 + reservedSize, exeSize, zexebuf);
	exeCompData->load(zexebuf, 0, exeSize);

	// Check program section CRC.
	if (exeCRC != crc32(crc32(0L, Z_NULL, 0), zexebuf, exeSize))
	{
		errorstr = L"CRC failure - executable data is corrupt";
		exeCompData->clear();
		return false;
	}

	// Read reserved section.
	BYTE* reservebuf = new BYTE[reservedSize ? reservedSize : 1];
	if (reservebuf == NULL)
	{
		errorstr = L"Out of memory reading the file";
		exeCompData->clear();
		return false;
	}
	file->GetBytes(16, reservedSize, reservebuf);
	reservedData->load(reservebuf, 0, reservedSize);

	// Check existence of tag section.
	UINT tagSectionOffset = 16 + reservedSize + exeSize;
	UINT tagSectionSize = fileSize - tagSectionOffset;
	bool hasTagSection = false;
	if (tagSectionSize >= PSF_TAG_SIG_LEN)
	{
		BYTE tagSig[5];
		file->GetBytes(tagSectionOffset, PSF_TAG_SIG_LEN, tagSig);
		if (memcmp(tagSig, PSF_TAG_SIG, PSF_TAG_SIG_LEN) == 0)
		{
			hasTagSection = true;
		}
	}

	// Check if tag field exists.
	char* tagSect = new char[tagSectionSize + 1];
	if(tagSect == NULL)
	{
		errorstr = L"Out of memory reading the file";
		exeCompData->clear();
		reservedData->clear();
		return false;
	}
	file->GetBytes(tagSectionOffset, tagSectionSize, tagSect);
	tagSect[tagSectionSize] = '\0';

	// Parse tag section. Details are available here:
	// http://wiki.neillcorlett.com/PSFTagFormat
	UINT tagCurPos = PSF_TAG_SIG_LEN;
	while (tagCurPos < tagSectionSize)
	{
		// Search the end position of the current line.
		char* pNewLine = strchr(&tagSect[tagCurPos], 0x0a);
		if (pNewLine == NULL)
		{
			// Tag section must end with a newline.
			// Read the all remaining bytes if a newline lacks though.
			pNewLine = tagSect + tagSectionSize;
		}

		// Replace the newline with NUL,
		// for better C string function compatibility.
		*pNewLine = '\0';

		// Search the variable=value separator.
		char* pSeparator = strchr(&tagSect[tagCurPos], '=');
		if (pSeparator == NULL)
		{
			// Blank lines, or lines not of the form "variable=value", are ignored.
			tagCurPos = pNewLine + 1 - tagSect;
			continue;
		}

		// Determine the start/end position of variable.
		char* pVarName = &tagSect[tagCurPos];
		char* pVarNameEnd = pSeparator;
		char* pVarVal = pSeparator + 1;
		char* pVarValEnd = pNewLine;

		// Whitespace at the beginning/end of the line and before/after the = are ignored.
		// All characters 0x01-0x20 are considered whitespace.
		// (There must be no null (0x00) characters.)
		// Trim them.
		while (pVarNameEnd > pVarName && *(unsigned char*)(pVarNameEnd - 1) <= 0x20)
			pVarNameEnd--;
		while (pVarValEnd > pVarVal && *(unsigned char*)(pVarValEnd - 1) <= 0x20)
			pVarValEnd--;
		while (pVarName < pVarNameEnd && *(unsigned char*)pVarName <= 0x20)
			pVarName++;
		while (pVarVal < pVarValEnd && *(unsigned char*)pVarVal <= 0x20)
			pVarVal++;

		// Read variable=value as string.
		string varName(pVarName, pVarNameEnd - pVarName);
		string varVal(pVarVal, pVarValEnd - pVarVal);

		// Multiple-line variables must appear as consecutive lines using the same variable name.
		// For instance:
		//   comment=This is a
		//   comment=multiple-line
		//   comment=comment.
		// Therefore, check if the variable had already appeared.
		map<string, string>::iterator it = tags.lower_bound(varName);
		if (it != tags.end() && it->first == varName)
		{
			it->second += "\n";
			it->second += varVal;
		}
		else
		{
			tags.insert(it, make_pair(varName, varVal));
		}

		tagCurPos = pNewLine + 1 - tagSect;
	}
	delete[] tagSect;

	return true;
}

bool PSFFile::ReadExe(BYTE* buf, size_t readlen) const
{
	uLong destlen = readlen;
	if (uncompress(buf, &destlen, exeCompData->data, exeCompData->size) == Z_DATA_ERROR)
	{
		//errorstr = L"Decompression failed";
		return false;
	}
	return true;
}

bool PSFFile::Decompress(size_t decompressed_size)
{
	if (decompressed_size == 0)
	{
		exeData->clear();
		if (exeCompData->size == 0)
		{
			return true;
		}
		else
		{
			// Mismatched length.
			errorstr = L"Decompression failed";
			return false;
		}
	}

	BYTE* buf = new BYTE[decompressed_size];
	if (buf == NULL)
	{
		errorstr = L"Out of memory reading the file";
		return false;
	}

	uLong destlen = decompressed_size;
	if (uncompress(buf, &destlen, exeCompData->data, exeCompData->size) != Z_OK)
	{
		errorstr = L"Decompression failed";
		delete[] buf;
		return false;
	}

	exeData->load(buf, 0, decompressed_size);
	decompressed = true;
	return true;
}

bool PSFFile::IsDecompressed() const
{
	return decompressed;
}

BYTE PSFFile::GetVersion(void) const
{
	return version;
}

size_t PSFFile::GetExeSize(void) const
{
	return exeData->size;
}

size_t PSFFile::GetCompressedExeSize(void) const
{
	return exeCompData->size;
}

size_t PSFFile::GetReservedSize(void) const
{
	return reservedData->size;
}

void PSFFile::Clear()
{
	exeData->clear();
	exeCompData->clear();
	reservedData->clear();
	tags.clear();
	version = 0;
	decompressed = false;
	errorstr = NULL;
	parent = NULL;
}

wchar_t* PSFFile::GetError(void) const
{
	return errorstr;
}
