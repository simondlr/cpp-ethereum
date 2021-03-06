/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file DappLoader.cpp
 * @author Arkadiy Paronyan <arkadiy@ethdev.org>
 * @date 2015
 */

#include <algorithm>
#include <json/json.h>
#include <QUrl>
#include <QStringList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <libdevcore/Common.h>
#include <libdevcore/RLP.h>
#include <libdevcrypto/CryptoPP.h>
#include <libdevcrypto/SHA3.h>
#include <libethcore/CommonJS.h>
#include <libethereum/Client.h>
#include <libwebthree/WebThree.h>
#include "DappLoader.h"

using namespace dev;
using namespace dev::eth;
using namespace dev::crypto;

Address c_registrar = Address("0000000000000000000000000000000000000a28");
Address c_urlHint = Address("0000000000000000000000000000000000000a29");

QString contentsOfQResource(std::string const& res);

DappLoader::DappLoader(QObject* _parent, WebThreeDirect* _web3):
	QObject(_parent), m_web3(_web3)
{
	connect(&m_net, &QNetworkAccessManager::finished, this, &DappLoader::downloadComplete);
}

DappLocation DappLoader::resolveAppUri(QString const& _uri)
{
	QUrl url(_uri);
	if (!url.scheme().isEmpty() && url.scheme() != "eth")
		throw dev::Exception(); //TODO:

	QStringList parts = url.host().split('.', QString::SkipEmptyParts);
	QStringList domainParts;
	std::reverse(parts.begin(), parts.end());
	parts.append(url.path().split('/', QString::SkipEmptyParts));

	Address address = c_registrar;
	Address lastAddress;
	int partIndex = 0;

	h256 contentHash;

	while (address && partIndex < parts.length())
	{
		lastAddress = address;
		string32 name = { 0 };
		QByteArray utf8 = parts[partIndex].toUtf8();
		std::copy(utf8.data(), utf8.data() + utf8.size(), name.data());
		address = abiOut<Address>(web3()->ethereum()->call(address, abiIn("addr(string32)", name)));
		domainParts.append(parts[partIndex]);
		if (!address)
		{
			//we have the address of the last part, try to get content hash
			contentHash = abiOut<h256>(web3()->ethereum()->call(lastAddress, abiIn("content(string32)", name)));
			if (!contentHash)
				throw dev::Exception() << errinfo_comment("Can't resolve address");
		}
		++partIndex;
	}


	string32 contentUrl = abiOut<string32>(web3()->ethereum()->call(c_urlHint, abiIn("url(hash256)", contentHash)));
	QString domain = domainParts.join('/');
	parts.erase(parts.begin(), parts.begin() + partIndex);
	QString path = parts.join('/');
	QString contentUrlString = QString::fromUtf8(std::string(contentUrl.data(), contentUrl.size()).c_str());
	if (!contentUrlString.startsWith("http://") || !contentUrlString.startsWith("https://"))
		contentUrlString = "http://" + contentUrlString;
	return DappLocation { domain, path, contentUrlString, contentHash };
}

void DappLoader::downloadComplete(QNetworkReply* _reply)
{
	try
	{
		//try to interpret as rlp
		QByteArray data = _reply->readAll();
		_reply->deleteLater();

		h256 expected = m_uriHashes[_reply->request().url()];
		bytes package(reinterpret_cast<unsigned char const*>(data.constData()), reinterpret_cast<unsigned char const*>(data.constData() + data.size()));
		Secp256k1 dec;
		dec.decrypt(expected, package);
		h256 got = sha3(package);
		if (got != expected)
		{
			//try base64
			data = QByteArray::fromBase64(data);
			package = bytes(reinterpret_cast<unsigned char const*>(data.constData()), reinterpret_cast<unsigned char const*>(data.constData() + data.size()));
			dec.decrypt(expected, package);
			got = sha3(package);
			if (got != expected)
				throw dev::Exception() << errinfo_comment("Dapp content hash does not match");
		}

		RLP rlp(package);
		loadDapp(rlp);
	}
	catch (...)
	{
		qWarning() << tr("Error downloading DApp: ") << boost::current_exception_diagnostic_information().c_str();
		emit dappError();
	}

}

void DappLoader::loadDapp(RLP const& _rlp)
{
	Dapp dapp;
	unsigned len = _rlp.itemCountStrict();
	dapp.manifest = loadManifest(_rlp[0].toString());
	for (unsigned c = 1; c < len; ++c)
	{
		bytesConstRef content = _rlp[c].toBytesConstRef();
		h256 hash = sha3(content);
		auto entry = std::find_if(dapp.manifest.entries.cbegin(), dapp.manifest.entries.cend(), [=](ManifestEntry const& _e) { return _e.hash == hash; });
		if (entry != dapp.manifest.entries.cend())
		{
			if (entry->path == "/deployment.js")
			{
				//inject web3 code
				QString code;
				code += contentsOfQResource(":/js/bignumber.min.js");
				code += "\n";
				code += contentsOfQResource(":/js/webthree.js");
				code += "\n";
				code += contentsOfQResource(":/js/setup.js");
				code += "\n";
				QByteArray res = code.toLatin1();
				bytes b(res.data(), res.data() + res.size());
				b.insert(b.end(), content.begin(), content.end());
				dapp.content[hash] = b;
			}
			else
				dapp.content[hash] = content.toBytes();
		}
		else
			throw dev::Exception() << errinfo_comment("Dapp content hash does not match");
	}
	emit dappReady(dapp);
}

Manifest DappLoader::loadManifest(std::string const& _manifest)
{
	/// https://github.com/ethereum/go-ethereum/wiki/URL-Scheme
	Manifest manifest;
	Json::Reader jsonReader;
	Json::Value root;
	jsonReader.parse(_manifest, root, false);

	Json::Value entries = root["entries"];
	for (Json::ValueIterator it = entries.begin(); it != entries.end(); ++it)
	{
		Json::Value const& entryValue = *it;
		std::string path = entryValue["path"].asString();
		if (path.size() == 0 || path[0] != '/')
			path = "/" + path;
		std::string contentType = entryValue["contentType"].asString();
		std::string strHash = entryValue["hash"].asString();
		if (strHash.length() == 64)
			strHash = "0x" + strHash;
		h256 hash = jsToFixed<32>(strHash);
		unsigned httpStatus = entryValue["status"].asInt();
		manifest.entries.push_back(ManifestEntry{ path, hash, contentType, httpStatus });
	}
	return manifest;
}

void DappLoader::loadDapp(QString const& _uri)
{
	DappLocation location = resolveAppUri(_uri);
	QUrl uri(location.contentUri);
	QNetworkRequest request(uri);
	m_uriHashes[uri] = location.contentHash;
	m_net.get(request);
}

