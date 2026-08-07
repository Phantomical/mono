// 
// TcpChannelListener.cs
// 
// Author: 
//     Marcos Cobena (marcoscobena@gmail.com)
//     Atsushi Enomoto  (atsushi@ximian.com)
// 
// Copyright 2007 Marcos Cobena (http://www.youcannoteatbits.org/)
// Copyright 2009-2010 Novell, Inc (http://www.novell.com/)
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// 
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//

using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.ServiceModel.Description;
using System.Text;
using System.Threading;
using System.Xml;

namespace System.ServiceModel.Channels.NetTcp
{
	internal class TcpChannelListener<TChannel> : InternalChannelListenerBase<TChannel> 
		where TChannel : class, IChannel
	{
		BindingContext context;
		TcpChannelInfo info;
		TcpListener tcp_listener;
		
		public TcpChannelListener (TcpTransportBindingElement source, BindingContext context)
			: base (context)
		{
			XmlDictionaryReaderQuotas quotas = null;

			foreach (BindingElement be in context.Binding.Elements) {
				MessageEncodingBindingElement mbe = be as MessageEncodingBindingElement;
				if (mbe != null) {
					MessageEncoder = CreateEncoder<TChannel> (mbe);
					quotas = mbe.GetProperty<XmlDictionaryReaderQuotas> (context);
					break;
				}
			}
			
			if (MessageEncoder == null)
				MessageEncoder = new BinaryMessageEncoder ();

			info = new TcpChannelInfo (source, MessageEncoder, quotas);
		}
		
		// Guards the queue of accepted clients together with the list of waiters on
		// it.  Looking at the queue and registering a waiter have to be one step: a
		// client queued in between signals nobody, and the waiter then sleeps out its
		// whole timeout with a connection already sitting there.
		object accept_lock = new object ();
		List<ManualResetEvent> accept_handles = new List<ManualResetEvent> ();
		Queue<TcpClient> accepted_clients = new Queue<TcpClient> ();
		SynchronizedCollection<TChannel> accepted_channels = new SynchronizedCollection<TChannel> ();

		protected override TChannel OnAcceptChannel (TimeSpan timeout)
		{
			DateTime start = DateTime.UtcNow;

			// Close channels that are incorrectly kept open first.
			var l = new List<TcpDuplexSessionChannel> ();
			foreach (var tch in accepted_channels) {
				var dch = tch as TcpDuplexSessionChannel;
				if (dch != null && dch.TcpClient != null && !dch.TcpClient.Connected)
					l.Add (dch);
			}
			foreach (var dch in l)
				dch.Close (timeout - (DateTime.UtcNow - start));

			TcpClient client = AcceptTcpClient (timeout - (DateTime.UtcNow - start));
			if (client == null)
				return null; // onclose

			TChannel ch;

			if (typeof (TChannel) == typeof (IDuplexSessionChannel))
				ch = (TChannel) (object) new TcpDuplexSessionChannel (this, info, client);
			else if (typeof (TChannel) == typeof (IReplyChannel))
				ch = (TChannel) (object) new TcpReplyChannel (this, info, client);
			else
				throw new InvalidOperationException (String.Format ("Channel type {0} is not supported.", typeof (TChannel).Name));

			((ChannelBase) (object) ch).Closed += delegate {
				accepted_channels.Remove (ch);
				};
			accepted_channels.Add (ch);

			return ch;
		}

		// TcpReplyChannel requires refreshed connection after each request processing.
		internal TcpClient AcceptTcpClient (TimeSpan timeout)
		{
			DateTime start = DateTime.UtcNow;

			while (true) {
				TcpClient client = null;
				ManualResetEvent wait = null;

				lock (accept_lock) {
					if (accepted_clients.Count > 0)
						client = accepted_clients.Dequeue ();
					else {
						wait = new ManualResetEvent (false);
						accept_handles.Add (wait);
					}
				}

				if (client == null) {
					var remaining = timeout - (DateTime.UtcNow - start);
					bool signaled = remaining > TimeSpan.Zero && wait.WaitOne (remaining);
					lock (accept_lock)
						accept_handles.Remove (wait);
					// timed out, or woken by the listener being closed.
					if (!signaled || State != CommunicationState.Opened)
						return null;
					continue;
				}

				// There might be better way to exclude those TCP clients though ...
				if (!IsAlreadyAccepted (client))
					return client;
				// ... it is handled in another BeginTryReceive/EndTryReceive loop in ChannelDispatcher.
			}
		}

		bool IsAlreadyAccepted (TcpClient client)
		{
			foreach (var ch in accepted_channels) {
				var dch = ch as TcpDuplexSessionChannel;
				if (dch == null || dch.TcpClient == null || !dch.TcpClient.Connected)
					continue;
				if (((IPEndPoint) dch.TcpClient.Client.RemoteEndPoint).Equals (client.Client.RemoteEndPoint))
					return true;
			}
			return false;
		}

		[MonoTODO]
		protected override bool OnWaitForChannel (TimeSpan timeout)
		{
			throw new NotImplementedException ();
		}
		
		// CommunicationObject
		
		protected override void OnAbort ()
		{
			if (State == CommunicationState.Closed)
				return;
			ProcessClose (TimeSpan.Zero);
		}

		protected override void OnClose (TimeSpan timeout)
		{
			if (State == CommunicationState.Closed)
				return;
			ProcessClose (timeout);
		}

		void ProcessClose (TimeSpan timeout)
		{
			if (tcp_listener == null)
				throw new InvalidOperationException ("Current state is " + State);
			//tcp_listener.Client.Close (Math.Max (50, (int) timeout.TotalMilliseconds));
			tcp_listener.Stop ();
			lock (accept_lock)
				foreach (var wait in accept_handles)
					wait.Set ();
			tcp_listener = null;
		}

		protected override void OnOpen (TimeSpan timeout)
		{
			IPAddress address;

			if (string.Equals (Uri.Host, "localhost", StringComparison.InvariantCultureIgnoreCase))
				address = IPAddress.Any;
			else {
				IPHostEntry entry = Dns.GetHostEntry (Uri.Host);
				if (entry.AddressList.Length == 0)
					throw new ArgumentException (String.Format ("Invalid listen URI: {0}", Uri));
				address = entry.AddressList [0];
			}
			
			int explicitPort = Uri.Port;
			tcp_listener = new TcpListener (address, explicitPort <= 0 ? TcpTransportBindingElement.DefaultPort : explicitPort);
			tcp_listener.Start ();
			tcp_listener.BeginAcceptTcpClient (TcpListenerAcceptedClient, tcp_listener);
		}

		void TcpListenerAcceptedClient (IAsyncResult result)
		{
			var listener = (TcpListener) result.AsyncState;
			try {
				var client = listener.EndAcceptTcpClient (result);
				if (client != null) {
					lock (accept_lock) {
						accepted_clients.Enqueue (client);
						// Waking every waiter costs a spurious pass around the loop
						// above, where waking only the first one drops a client
						// whenever two arrive before that waiter has dequeued.
						foreach (var wait in accept_handles)
							wait.Set ();
					}
				}
			} catch (ObjectDisposedException) {
				/* If an accept fails, just ignore it. Maybe the remote peer disconnected already */
			} finally {
				if (State == CommunicationState.Opened) {
					try {
						listener.BeginAcceptTcpClient (TcpListenerAcceptedClient, listener);
					} catch (ObjectDisposedException) {
						/* If this fails, we must have disposed the listener */
					}
				}
			}
		}
	}
}

